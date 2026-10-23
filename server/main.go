package main

import (
	"encoding/json"
	"errors"
	"flag"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const (
	maxFrame       = 4096
	maxFrames      = 500
	maxBytes       = 512 * 1024
	maxControl     = 1024
	maxConnections = 128
	writeTimeout   = 3 * time.Second
	readyTimeout   = 2 * time.Second
)

var validID = regexp.MustCompile(`^[A-Za-z0-9_-]{1,32}$`)

type control struct {
	Type       string `json:"type"`
	Version    int    `json:"version,omitempty"`
	DeviceID   string `json:"device_id,omitempty"`
	SourceID   string `json:"source_id,omitempty"`
	MessageID  string `json:"message_id,omitempty"`
	FrameMS    int    `json:"frame_ms,omitempty"`
	SampleRate int    `json:"sample_rate,omitempty"`
	FrameCount int    `json:"frame_count,omitempty"`
	OK         *bool  `json:"ok,omitempty"`
	Error      string `json:"error,omitempty"`
	Receivers  int    `json:"receivers,omitempty"`
}

func answer(t, id string, ok bool, reason string) control {
	return control{Type: t, MessageID: id, OK: &ok, Error: reason}
}

type outgoing struct {
	kind int
	data []byte
}

type peer struct {
	id        string
	conn      *websocket.Conn
	out       chan outgoing
	stop      chan struct{}
	closeOnce sync.Once
	srv       *server
	mu        sync.Mutex
	receiving string
	ready     bool
	sending   string
	frames    int
	bytes     int
	recording *wavRecording
}

func (p *peer) enqueue(kind int, data []byte) bool {
	select {
	case p.out <- outgoing{kind, append([]byte(nil), data...)}:
		return true
	default:
		return false
	}
}

func (p *peer) send(c control) bool {
	b, _ := json.Marshal(c)
	return p.enqueue(websocket.TextMessage, b)
}

func (p *peer) close() {
	p.closeOnce.Do(func() {
		close(p.stop)
		p.conn.Close()
		p.srv.remove(p)
	})
}

func (p *peer) writer() {
	ticker := time.NewTicker(20 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case msg := <-p.out:
			p.conn.SetWriteDeadline(time.Now().Add(writeTimeout))
			if err := p.conn.WriteMessage(msg.kind, msg.data); err != nil {
				log.Printf("write failed id=%s kind=%d: %v", p.id, msg.kind, err)
				p.close()
				return
			}
		case <-ticker.C:
			p.conn.SetWriteDeadline(time.Now().Add(writeTimeout))
			if err := p.conn.WriteControl(websocket.PingMessage, nil, time.Now().Add(writeTimeout)); err != nil {
				log.Printf("ping failed id=%s: %v", p.id, err)
				p.close()
				return
			}
		case <-p.stop:
			return
		}
	}
}

type server struct {
	mu        sync.Mutex
	peers     map[string]*peer
	active    *peer
	started   time.Time
	slots     chan struct{}
	saveWAV   bool
	recordDir string
}

func newServer() *server {
	return &server{peers: make(map[string]*peer), slots: make(chan struct{}, maxConnections)}
}

func (s *server) remove(p *peer) {
	s.mu.Lock()
	if s.peers[p.id] == p {
		delete(s.peers, p.id)
	}
	if s.active == p {
		s.discardRecording(p)
		s.active = nil
		for _, other := range s.peers {
			other.mu.Lock()
			if other.receiving == p.sending {
				other.send(control{Type: "abort", MessageID: p.sending})
				other.receiving = ""
				other.ready = false
			}
			other.mu.Unlock()
		}
	}
	online := len(s.peers)
	s.mu.Unlock()
	log.Printf("disconnect id=%s online=%d", p.id, online)
}

func (s *server) count() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.peers)
}

func (s *server) register(p *peer, id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !validID.MatchString(id) || s.peers[id] != nil || len(s.peers) >= maxConnections {
		return errors.New("invalid, duplicate, or capacity-limited device_id")
	}
	p.id = id
	s.peers[id] = p
	log.Printf("connect id=%s online=%d", id, len(s.peers))
	return nil
}

func (s *server) start(p *peer, c control) {
	if c.MessageID == "" || len(c.MessageID) > 64 || c.FrameMS != 60 || c.SampleRate != 16000 {
		log.Printf("start rejected id=%s message=%s reason=invalid_parameters frame_ms=%d sample_rate=%d", p.id, c.MessageID, c.FrameMS, c.SampleRate)
		p.send(answer("start_ack", c.MessageID, false, "invalid start"))
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.active != nil {
		log.Printf("start rejected id=%s message=%s reason=busy active_id=%s", p.id, c.MessageID, s.active.id)
		p.send(answer("start_ack", c.MessageID, false, "busy"))
		return
	}
	s.active = p
	s.started = time.Now()
	p.sending, p.frames, p.bytes = c.MessageID, 0, 0
	if s.saveWAV {
		if err := s.beginRecording(p); err != nil {
			log.Printf("debug WAV unavailable id=%s message=%s: %v", p.id, c.MessageID, err)
		}
	}
	offered := 0
	for _, other := range s.peers {
		if other == p {
			continue
		}
		other.mu.Lock()
		// Realtime talk starts forwarding immediately. A receiver that cannot
		// accept audio can still revoke the offer with a busy response.
		other.receiving, other.ready = c.MessageID, true
		if !other.send(control{Type: "start", SourceID: p.id, MessageID: c.MessageID, FrameMS: 60, SampleRate: 16000}) {
			log.Printf("receive start queue full id=%s message=%s", other.id, c.MessageID)
			other.receiving = ""
		} else {
			offered++
		}
		other.mu.Unlock()
	}
	started := s.started
	if !p.send(answer("start_ack", c.MessageID, true, "")) {
		log.Printf("start ack queue full id=%s message=%s", p.id, c.MessageID)
	}
	time.AfterFunc(35*time.Second, func() {
		s.mu.Lock()
		stalled := s.active == p && s.started.Equal(started)
		s.mu.Unlock()
		if stalled {
			log.Printf("upload timeout id=%s message=%s", p.id, c.MessageID)
			p.close()
		}
	})
	log.Printf("start id=%s message=%s offered=%d ack=immediate", p.id, c.MessageID, offered)
}

func (s *server) frame(p *peer, data []byte) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.active != p || len(data) == 0 || len(data) > maxFrame || p.frames >= maxFrames || p.bytes+len(data) > maxBytes || time.Since(s.started) > 35*time.Second {
		return errors.New("invalid or oversized audio frame")
	}
	p.frames++
	p.bytes += len(data)
	if p.recording != nil {
		if err := p.recording.appendOpus(data); err != nil {
			log.Printf("debug WAV failed id=%s message=%s: %v", p.id, p.sending, err)
			s.discardRecording(p)
		}
	}
	forwarded := 0
	for _, other := range s.peers {
		other.mu.Lock()
		if other != p && other.receiving == p.sending && other.ready {
			if !other.enqueue(websocket.BinaryMessage, data) {
				other.receiving = ""
				other.ready = false
				log.Printf("slow receiver id=%s", other.id)
				go other.close()
			} else {
				forwarded++
			}
		}
		other.mu.Unlock()
	}
	if p.frames == 1 || p.frames%50 == 0 {
		log.Printf("audio frame id=%s message=%s frame=%d bytes=%d forwarded=%d", p.id, p.sending, p.frames, p.bytes, forwarded)
	}
	return nil
}

func (s *server) end(p *peer, c control) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.active != p || p.sending != c.MessageID || c.FrameCount != p.frames {
		return errors.New("invalid end or frame count")
	}
	receivers := 0
	for _, other := range s.peers {
		other.mu.Lock()
		if other != p && other.receiving == c.MessageID {
			if other.ready && other.send(control{Type: "end", MessageID: c.MessageID, FrameCount: p.frames}) {
				receivers++
				log.Printf("receive end queued id=%s message=%s frames=%d", other.id, c.MessageID, p.frames)
			} else {
				log.Printf("receive end skipped id=%s message=%s ready=%t", other.id, c.MessageID, other.ready)
			}
			other.receiving, other.ready = "", false
		}
		other.mu.Unlock()
	}
	p.send(control{Type: "end_ack", MessageID: c.MessageID, OK: boolPtr(true), Receivers: receivers})
	if p.recording != nil {
		if err := p.recording.finish(); err != nil {
			log.Printf("debug WAV finalize failed id=%s message=%s: %v", p.id, c.MessageID, err)
			s.discardRecording(p)
		} else {
			log.Printf("debug WAV saved path=%s", p.recording.path)
			p.recording = nil
		}
	}
	log.Printf("end id=%s message=%s frames=%d bytes=%d receivers=%d duration=%s", p.id, c.MessageID, p.frames, p.bytes, receivers, time.Since(s.started).Round(time.Millisecond))
	p.sending = ""
	s.active = nil
	return nil
}

func boolPtr(v bool) *bool { return &v }

func (s *server) abort(p *peer) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.active != p {
		return
	}
	log.Printf("abort id=%s message=%s frames=%d bytes=%d", p.id, p.sending, p.frames, p.bytes)
	s.discardRecording(p)
	for _, other := range s.peers {
		other.mu.Lock()
		if other != p && other.receiving == p.sending {
			other.send(control{Type: "abort", MessageID: p.sending})
			other.receiving, other.ready = "", false
		}
		other.mu.Unlock()
	}
	p.sending = ""
	s.active = nil
}

func (s *server) handle(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/ws" {
		http.NotFound(w, r)
		return
	}
	select {
	case s.slots <- struct{}{}:
		defer func() { <-s.slots }()
	default:
		http.Error(w, "connection limit reached", http.StatusServiceUnavailable)
		return
	}
	conn, err := (&websocket.Upgrader{CheckOrigin: func(r *http.Request) bool { return r.Header.Get("Origin") == "" }}).Upgrade(w, r, nil)
	if err != nil {
		return
	}
	p := &peer{conn: conn, out: make(chan outgoing, 16), stop: make(chan struct{}), srv: s}
	defer p.close()
	conn.SetReadLimit(maxFrame + maxControl)
	conn.SetReadDeadline(time.Now().Add(30 * time.Second))
	conn.SetPongHandler(func(string) error { return conn.SetReadDeadline(time.Now().Add(45 * time.Second)) })
	kind, data, err := conn.ReadMessage()
	if err != nil || kind != websocket.TextMessage || len(data) > maxControl {
		return
	}
	var hello control
	if json.Unmarshal(data, &hello) != nil || hello.Type != "hello" || hello.Version != 1 || s.register(p, hello.DeviceID) != nil {
		conn.WriteJSON(answer("hello_ack", "", false, "invalid version or device_id"))
		return
	}
	go p.writer()
	p.send(answer("hello_ack", "", true, ""))
	for {
		kind, data, err = conn.ReadMessage()
		if err != nil {
			log.Printf("read ended id=%s: %v", p.id, err)
			return
		}
		if kind == websocket.BinaryMessage {
			if err = s.frame(p, data); err != nil {
				log.Printf("audio frame rejected id=%s bytes=%d: %v", p.id, len(data), err)
				p.send(control{Type: "error", Error: err.Error()})
				s.abort(p)
			}
			continue
		}
		if kind != websocket.TextMessage || len(data) > maxControl {
			return
		}
		var c control
		if json.Unmarshal(data, &c) != nil {
			return
		}
		switch c.Type {
		case "start":
			s.start(p, c)
		case "receive_ready", "busy":
			s.mu.Lock()
			withinReadyWindow := time.Since(s.started) <= readyTimeout
			elapsed := time.Since(s.started).Round(time.Millisecond)
			s.mu.Unlock()
			p.mu.Lock()
			if p.receiving == c.MessageID {
				p.ready = c.Type == "receive_ready" && withinReadyWindow
				log.Printf("receiver response id=%s message=%s type=%s accepted=%t elapsed=%s", p.id, c.MessageID, c.Type, p.ready, elapsed)
				if !p.ready {
					p.receiving = ""
				}
			} else {
				log.Printf("receiver response ignored id=%s message=%s type=%s pending=%s", p.id, c.MessageID, c.Type, p.receiving)
			}
			p.mu.Unlock()
		case "end":
			if err := s.end(p, c); err != nil {
				log.Printf("end rejected id=%s message=%s frames=%d: %v", p.id, c.MessageID, c.FrameCount, err)
				p.send(control{Type: "error", Error: err.Error()})
				s.abort(p)
			}
		case "abort":
			s.abort(p)
		default:
			p.send(control{Type: "error", Error: "unknown message type"})
			s.abort(p)
		}
	}
}

func main() {
	addr := flag.String("listen", "127.0.0.1:8080", "listen address")
	saveWAV := flag.Bool("save-wav", true, "save complete uploads as debug WAV files")
	recordDir := flag.String("record-dir", "recordings", "debug WAV directory")
	flag.Parse()
	s := newServer()
	s.saveWAV = *saveWAV
	if *saveWAV {
		absoluteDir, err := filepath.Abs(*recordDir)
		if err != nil {
			log.Fatal(err)
		}
		if err := os.MkdirAll(absoluteDir, 0700); err != nil {
			log.Fatal(err)
		}
		s.recordDir = absoluteDir
		log.Printf("debug WAV enabled dir=%s", absoluteDir)
	}
	log.Printf("intercom listening on %s", *addr)
	log.Fatal(http.ListenAndServe(*addr, httpHandler(s)))
}

func httpHandler(s *server) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", s.handle)
	return mux
}
