package main

import (
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

type room struct {
	ID        string   `json:"id"`
	Name      string   `json:"name"`
	JoinCode  string   `json:"join_code,omitempty"`
	CreatedBy string   `json:"created_by_device_id,omitempty"`
	Default   bool     `json:"is_default"`
	Closed    bool     `json:"closed"`
	Devices   []string `json:"device_ids"`
	Invited   []string `json:"invite_device_ids,omitempty"`
}
type device struct {
	ID       string `json:"device_id"`
	LastSeen string `json:"last_seen_at"`
	IP       string `json:"last_ip"`
	Online   bool   `json:"online"`
	RoomID   string `json:"room_id"`
}

type roomStore interface {
	RegisterDevice(id, ip string) room
	CurrentRoom(deviceID string) room
	Join(deviceID, code string) (room, error)
	Leave(deviceID string) room
	CreateRoom(name, creator string, invited []string) (room, error)
	ListRooms() []room
	RoomDevices(id string) []device
	CloseRoom(id string) error
	ReturnDefault(id string) error
	ListDevices() []device
}

type memoryStore struct {
	mu      sync.Mutex
	rooms   map[string]*room
	devices map[string]*device
}

func newMemoryStore() *memoryStore {
	return &memoryStore{rooms: map[string]*room{"default": {ID: "default", Name: "默认房间", Default: true}}, devices: map[string]*device{}}
}
func (s *memoryStore) RegisterDevice(id, ip string) room {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.devices[id]
	if d == nil {
		d = &device{ID: id}
		s.devices[id] = d
	}
	d.LastSeen = time.Now().UTC().Format(time.RFC3339)
	d.IP = ip
	d.Online = true
	return s.currentLocked(id)
}
func (s *memoryStore) currentLocked(id string) room {
	if d := s.devices[id]; d != nil && d.RoomID != "" {
		if r := s.rooms[d.RoomID]; r != nil && !r.Closed {
			return cloneRoom(r)
		}
	}
	return cloneRoom(s.rooms["default"])
}
func cloneRoom(r *room) room {
	if r == nil {
		return room{}
	}
	x := *r
	x.Devices = append([]string(nil), r.Devices...)
	return x
}
func (s *memoryStore) CurrentRoom(id string) room {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.currentLocked(id)
}
func (s *memoryStore) Join(id, code string) (room, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, r := range s.rooms {
		if r.JoinCode == code && !r.Closed {
			if len(r.Invited) > 0 && !contains(r.Invited, id) && r.CreatedBy != id {
				return room{}, errors.New("device is not invited")
			}
			s.setRoomLocked(id, r.ID)
			return cloneRoom(r), nil
		}
	}
	return room{}, errors.New("invalid room code")
}
func (s *memoryStore) Leave(id string) room {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.setRoomLocked(id, "default")
	return cloneRoom(s.rooms["default"])
}
func (s *memoryStore) setRoomLocked(id, rid string) {
	d := s.devices[id]
	if d == nil {
		d = &device{ID: id}
		s.devices[id] = d
	}
	for _, r := range s.rooms {
		r.Devices = remove(r.Devices, id)
	}
	d.RoomID = rid
	if rid != "default" {
		s.rooms[rid].Devices = append(s.rooms[rid].Devices, id)
	}
}
func remove(xs []string, id string) []string {
	out := xs[:0]
	for _, x := range xs {
		if x != id {
			out = append(out, x)
		}
	}
	return out
}
func (s *memoryStore) CreateRoom(name, creator string, invited []string) (room, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if strings.TrimSpace(name) == "" {
		return room{}, errors.New("name is required")
	}
	idb := make([]byte, 8)
	_, _ = rand.Read(idb)
	id := "room-" + hex.EncodeToString(idb)
	code := ""
	for {
		b := make([]byte, 4)
		_, _ = rand.Read(b)
		code = fmt.Sprintf("%06d", uint32(b[0])<<16|uint32(b[1])<<8|uint32(b[2]))[:6]
		found := false
		for _, r := range s.rooms {
			if r.JoinCode == code {
				found = true
			}
		}
		if !found {
			break
		}
	}
	r := &room{ID: id, Name: strings.TrimSpace(name), JoinCode: code, CreatedBy: creator, Invited: append([]string(nil), invited...)}
	s.rooms[id] = r
	s.setRoomLocked(creator, id)
	return cloneRoom(r), nil
}
func contains(xs []string, v string) bool {
	for _, x := range xs {
		if x == v {
			return true
		}
	}
	return false
}
func (s *memoryStore) ListRooms() []room {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := []room{}
	for _, r := range s.rooms {
		if !r.Closed {
			out = append(out, cloneRoom(r))
		}
	}
	return out
}
func (s *memoryStore) RoomDevices(id string) []device {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := []device{}
	for did, d := range s.devices {
		if s.currentLocked(did).ID == id {
			x := *d
			x.RoomID = id
			out = append(out, x)
		}
	}
	return out
}
func (s *memoryStore) CloseRoom(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.rooms[id]
	if r == nil || r.Default {
		return errors.New("cannot close default room")
	}
	r.Closed = true
	for _, d := range s.devices {
		if d.RoomID == id {
			d.RoomID = ""
		}
	}
	return nil
}
func (s *memoryStore) ReturnDefault(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.setRoomLocked(id, "default")
	return nil
}
func (s *memoryStore) ListDevices() []device {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]device, 0, len(s.devices))
	for _, d := range s.devices {
		x := *d
		x.RoomID = s.currentLocked(d.ID).ID
		out = append(out, x)
	}
	return out
}

type sqliteStore struct {
	path string
	*memoryStore
}

func newSQLiteStore(path string) (*sqliteStore, error) {
	if dir := filepath.Dir(path); dir != "." {
		if err := os.MkdirAll(dir, 0700); err != nil {
			return nil, err
		}
	}
	s := &sqliteStore{path: path, memoryStore: newMemoryStore()}
	if _, err := s.exec(`PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS devices(device_id TEXT PRIMARY KEY,last_seen_at TEXT,ip TEXT,room_id TEXT); CREATE TABLE IF NOT EXISTS rooms(id TEXT PRIMARY KEY,name TEXT,join_code TEXT UNIQUE,is_default INTEGER,status TEXT,created_by TEXT,invite_device_ids TEXT DEFAULT '');`); err != nil {
		return nil, err
	}
	_, _ = s.exec(`ALTER TABLE rooms ADD COLUMN invite_device_ids TEXT DEFAULT ''`)
	_, err := s.exec(`INSERT OR IGNORE INTO rooms(id,name,join_code,is_default,status,created_by) VALUES('default','默认房间','',1,'active','')`)
	if err == nil {
		s.load()
	}
	return s, err
}
func (s *sqliteStore) exec(q string) ([]byte, error) {
	cmd := exec.Command("sqlite3", "-json", s.path, q)
	out, err := cmd.Output()
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok && len(ee.Stderr) > 0 {
			return out, fmt.Errorf("sqlite: %s", strings.TrimSpace(string(ee.Stderr)))
		}
	}
	return out, err
}

// sqliteString returns a SQL string literal. Go's %q is not suitable here:
// its double quotes are parsed by SQLite as identifiers.
func sqliteString(v string) string { return "'" + strings.ReplaceAll(v, "'", "''") + "'" }

func (s *sqliteStore) persistDevice(id string) {
	d := s.devices[id]
	if d == nil {
		return
	}
	q := fmt.Sprintf("INSERT INTO devices(device_id,last_seen_at,ip,room_id) VALUES(%s,%s,%s,%s) ON CONFLICT(device_id) DO UPDATE SET last_seen_at=excluded.last_seen_at,ip=excluded.ip,room_id=excluded.room_id", sqliteString(d.ID), sqliteString(d.LastSeen), sqliteString(d.IP), sqliteString(d.RoomID))
	if _, err := s.exec(q); err != nil {
		log.Printf("debug sqlite persist device=%s error=%v", id, err)
	}
}
func (s *sqliteStore) load() {
	log.Printf("debug sqlite loading path=%s", s.path)
	if b, err := s.exec("SELECT id,name,join_code,is_default,status,created_by,invite_device_ids FROM rooms"); err == nil && len(strings.TrimSpace(string(b))) > 0 {
		var rows []struct {
			ID      string `json:"id"`
			Name    string `json:"name"`
			Code    string `json:"join_code"`
			Default int    `json:"is_default"`
			Status  string `json:"status"`
			Creator string `json:"created_by"`
			Invited string `json:"invite_device_ids"`
		}
		if e := json.Unmarshal(b, &rows); e != nil {
			log.Printf("debug sqlite load rooms error=%v output=%s", e, b)
		} else {
			log.Printf("debug sqlite loaded rooms=%d", len(rows))
			for _, row := range rows {
				if row.ID != "default" {
					s.rooms[row.ID] = &room{ID: row.ID, Name: row.Name, JoinCode: row.Code, CreatedBy: row.Creator, Invited: splitIDs(row.Invited), Closed: row.Status != "active"}
				}
			}
		}
	} else {
		log.Printf("debug sqlite load rooms error=%v", err)
	}
	if b, err := s.exec("SELECT device_id,last_seen_at,ip,COALESCE(room_id,'') FROM devices"); err == nil && len(strings.TrimSpace(string(b))) > 0 {
		var rows []struct {
			ID     string `json:"device_id"`
			Seen   string `json:"last_seen_at"`
			IP     string `json:"ip"`
			RoomID string `json:"room_id"`
		}
		if e := json.Unmarshal(b, &rows); e != nil {
			log.Printf("debug sqlite load devices error=%v output=%s", e, b)
		} else {
			log.Printf("debug sqlite loaded devices=%d", len(rows))
			for _, row := range rows {
				s.devices[row.ID] = &device{ID: row.ID, LastSeen: row.Seen, IP: row.IP, RoomID: row.RoomID}
				if row.RoomID != "" && s.rooms[row.RoomID] != nil {
					s.rooms[row.RoomID].Devices = append(s.rooms[row.RoomID].Devices, row.ID)
				}
			}
		}
	} else {
		log.Printf("debug sqlite load devices error=%v", err)
	}
}
func splitIDs(v string) []string {
	if v == "" {
		return nil
	}
	return strings.Split(v, ",")
}
func (s *sqliteStore) RegisterDevice(id, ip string) room {
	r := s.memoryStore.RegisterDevice(id, ip)
	s.persistDevice(id)
	return r
}
func (s *sqliteStore) Join(id, code string) (room, error) {
	r, e := s.memoryStore.Join(id, code)
	if e == nil {
		s.persistDevice(id)
	}
	return r, e
}
func (s *sqliteStore) Leave(id string) room {
	r := s.memoryStore.Leave(id)
	s.persistDevice(id)
	return r
}
func (s *sqliteStore) CreateRoom(name, creator string, invited []string) (room, error) {
	r, e := s.memoryStore.CreateRoom(name, creator, invited)
	if e == nil {
		q := fmt.Sprintf("INSERT INTO rooms(id,name,join_code,is_default,status,created_by,invite_device_ids) VALUES(%s,%s,%s,0,'active',%s,%s)", sqliteString(r.ID), sqliteString(r.Name), sqliteString(r.JoinCode), sqliteString(r.CreatedBy), sqliteString(strings.Join(r.Invited, ",")))
		if _, err := s.exec(q); err != nil {
			log.Printf("debug sqlite create room=%s error=%v", r.ID, err)
			return room{}, err
		}
		for _, id := range r.Devices {
			s.persistDevice(id)
		}
	}
	return r, e
}
func (s *sqliteStore) CloseRoom(id string) error {
	e := s.memoryStore.CloseRoom(id)
	if e == nil {
		_, _ = s.exec(fmt.Sprintf("UPDATE rooms SET status='closed' WHERE id=%s", sqliteString(id)))
	}
	return e
}
func (s *sqliteStore) ReturnDefault(id string) error {
	e := s.memoryStore.ReturnDefault(id)
	s.persistDevice(id)
	return e
}

var _ = sql.ErrNoRows
