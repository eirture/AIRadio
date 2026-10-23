package main

import (
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func connect(t *testing.T, url, id string) *websocket.Conn {
	t.Helper()
	c, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	if err := c.WriteJSON(control{Type: "hello", Version: 1, DeviceID: id}); err != nil {
		t.Fatal(err)
	}
	ack := readControl(t, c)
	if ack.Type != "hello_ack" || ack.OK == nil || !*ack.OK {
		t.Fatalf("hello: %+v", ack)
	}
	return c
}

func readControl(t *testing.T, c *websocket.Conn) control {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	var msg control
	if err := c.ReadJSON(&msg); err != nil {
		t.Fatal(err)
	}
	return msg
}

func TestBroadcastAndBusy(t *testing.T) {
	s := newServer()
	http := httptest.NewServer(httpHandler(s))
	defer http.Close()
	url := "ws" + strings.TrimPrefix(http.URL, "http") + "/ws"
	a := connect(t, url, "truck-a")
	b := connect(t, url, "truck-b")
	c := connect(t, url, "truck-c")
	if err := a.WriteJSON(control{Type: "start", MessageID: "m1", FrameMS: 60, SampleRate: 16000}); err != nil {
		t.Fatal(err)
	}
	if got := readControl(t, b); got.Type != "start" || got.SourceID != "truck-a" {
		t.Fatalf("start: %+v", got)
	}
	if got := readControl(t, c); got.Type != "start" {
		t.Fatalf("start c: %+v", got)
	}
	b.WriteJSON(control{Type: "receive_ready", MessageID: "m1"})
	c.WriteJSON(control{Type: "busy", MessageID: "m1"})
	if err := b.WriteJSON(control{Type: "start", MessageID: "m2", FrameMS: 60, SampleRate: 16000}); err != nil {
		t.Fatal(err)
	}
	if got := readControl(t, b); got.Type != "start_ack" || got.OK == nil || *got.OK {
		t.Fatalf("busy: %+v", got)
	}
	if got := readControl(t, a); got.Type != "start_ack" || got.OK == nil || !*got.OK {
		t.Fatalf("ack: %+v", got)
	}
	if err := a.WriteMessage(websocket.BinaryMessage, []byte{1, 2, 3}); err != nil {
		t.Fatal(err)
	}
	b.SetReadDeadline(time.Now().Add(3 * time.Second))
	kind, frame, err := b.ReadMessage()
	if err != nil || kind != websocket.BinaryMessage || len(frame) != 3 {
		t.Fatalf("frame: %d %v %v", kind, frame, err)
	}
	a.WriteJSON(control{Type: "end", MessageID: "m1", FrameCount: 1})
	if got := readControl(t, b); got.Type != "end" || got.FrameCount != 1 {
		t.Fatalf("end: %+v", got)
	}
	if got := readControl(t, a); got.Type != "end_ack" || got.Receivers != 1 {
		t.Fatalf("end ack: %+v", got)
	}
}

func TestReceiverReadyAfterOneSecond(t *testing.T) {
	s := newServer()
	http := httptest.NewServer(httpHandler(s))
	defer http.Close()
	url := "ws" + strings.TrimPrefix(http.URL, "http") + "/ws"
	sender := connect(t, url, "sender")
	receiver := connect(t, url, "receiver")
	if err := sender.WriteJSON(control{Type: "start", MessageID: "slow-ready", FrameMS: 60, SampleRate: 16000}); err != nil {
		t.Fatal(err)
	}
	if got := readControl(t, receiver); got.Type != "start" {
		t.Fatalf("start: %+v", got)
	}
	time.Sleep(time.Second)
	if err := receiver.WriteJSON(control{Type: "receive_ready", MessageID: "slow-ready"}); err != nil {
		t.Fatal(err)
	}
	if got := readControl(t, sender); got.Type != "start_ack" || got.OK == nil || !*got.OK {
		t.Fatalf("start ack: %+v", got)
	}
	if err := sender.WriteMessage(websocket.BinaryMessage, []byte{1, 2, 3}); err != nil {
		t.Fatal(err)
	}
	receiver.SetReadDeadline(time.Now().Add(3 * time.Second))
	kind, frame, err := receiver.ReadMessage()
	if err != nil || kind != websocket.BinaryMessage || len(frame) != 3 {
		t.Fatalf("frame: kind=%d data=%v err=%v", kind, frame, err)
	}
	if err := sender.WriteJSON(control{Type: "end", MessageID: "slow-ready", FrameCount: 1}); err != nil {
		t.Fatal(err)
	}
	if got := readControl(t, receiver); got.Type != "end" {
		t.Fatalf("end: %+v", got)
	}
	if got := readControl(t, sender); got.Type != "end_ack" || got.Receivers != 1 {
		t.Fatalf("end ack: %+v", got)
	}
}

func TestDuplicateID(t *testing.T) {
	s := newServer()
	http := httptest.NewServer(httpHandler(s))
	defer http.Close()
	url := "ws" + strings.TrimPrefix(http.URL, "http") + "/ws"
	connect(t, url, "same")
	c, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.WriteJSON(control{Type: "hello", Version: 1, DeviceID: "same"})
	if got := readControl(t, c); got.OK == nil || *got.OK {
		t.Fatalf("duplicate: %+v", got)
	}
}

func TestBadFrameAbortsAndReleasesBroadcast(t *testing.T) {
	s := newServer()
	http := httptest.NewServer(httpHandler(s))
	defer http.Close()
	url := "ws" + strings.TrimPrefix(http.URL, "http") + "/ws"
	a := connect(t, url, "sender")
	b := connect(t, url, "receiver")
	a.WriteJSON(control{Type: "start", MessageID: "bad", FrameMS: 60, SampleRate: 16000})
	if got := readControl(t, b); got.Type != "start" {
		t.Fatalf("start: %+v", got)
	}
	b.WriteJSON(control{Type: "receive_ready", MessageID: "bad"})
	if got := readControl(t, a); got.Type != "start_ack" {
		t.Fatalf("ack: %+v", got)
	}
	a.WriteMessage(websocket.BinaryMessage, make([]byte, maxFrame+1))
	if got := readControl(t, a); got.Type != "error" {
		t.Fatalf("error: %+v", got)
	}
	if got := readControl(t, b); got.Type != "abort" {
		t.Fatalf("abort: %+v", got)
	}
	b.WriteJSON(control{Type: "start", MessageID: "next", FrameMS: 60, SampleRate: 16000})
	if got := readControl(t, a); got.Type != "start" {
		t.Fatalf("next start: %+v", got)
	}
	if got := readControl(t, b); got.Type != "start_ack" || got.OK == nil || !*got.OK {
		t.Fatalf("next ack: %+v", got)
	}
}
