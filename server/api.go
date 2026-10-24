package main

import (
	"encoding/json"
	"log"
	"net/http"
	"strings"
)

type apiServer struct {
	s        *server
	sessions map[string]bool
}

func (a *apiServer) json(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}
func (a *apiServer) auth(w http.ResponseWriter, r *http.Request) bool {
	c, e := r.Cookie("airadio_admin")
	if e != nil || !a.sessions[c.Value] {
		http.Error(w, "unauthorized", 401)
		return false
	}
	return true
}
func (a *apiServer) login(w http.ResponseWriter, r *http.Request) {
	var x struct{ Username, Password string }
	if json.NewDecoder(r.Body).Decode(&x) != nil || x.Username != "admin" || x.Password != "admin" {
		http.Error(w, "invalid credentials", 401)
		return
	}
	a.sessions["admin-session"] = true
	http.SetCookie(w, &http.Cookie{Name: "airadio_admin", Value: "admin-session", Path: "/"})
	a.json(w, map[string]bool{"ok": true})
}
func (a *apiServer) rooms(w http.ResponseWriter, r *http.Request) {
	if !a.auth(w, r) {
		return
	}
	if r.Method == "GET" {
		a.json(w, legacyRooms(a.s.store.ListRooms()))
		return
	}
	if r.Method == "POST" {
		var x struct {
			Name              string   `json:"name"`
			CreatedByDeviceID string   `json:"created_by_device_id"`
			DeviceIDs         []string `json:"device_ids"`
		}
		if json.NewDecoder(r.Body).Decode(&x) != nil {
			http.Error(w, "invalid json", 400)
			return
		}
		rr, e := a.s.store.CreateRoom(x.Name, x.CreatedByDeviceID, x.DeviceIDs)
		if e != nil {
			http.Error(w, e.Error(), 400)
			return
		}
		a.json(w, legacyRoom(rr))
		return
	}
	http.Error(w, "method not allowed", 405)
}

func legacyRoom(r room) map[string]any {
	return map[string]any{"id": r.ID, "name": r.Name, "join_code": r.JoinCode, "is_default": r.Default, "device_ids": r.Devices, "ID": r.ID, "JoinCode": r.JoinCode, "Default": r.Default, "Devices": r.Devices}
}
func legacyRooms(rs []room) []map[string]any {
	out := make([]map[string]any, 0, len(rs))
	for _, r := range rs {
		out = append(out, legacyRoom(r))
	}
	return out
}
func (a *apiServer) roomAction(w http.ResponseWriter, r *http.Request) {
	if !a.auth(w, r) {
		return
	}
	p := strings.Split(strings.Trim(r.URL.Path, "/"), "/")
	if len(p) < 3 {
		http.NotFound(w, r)
		return
	}
	id := p[2]
	if len(p) == 4 && p[3] == "devices" {
		a.json(w, a.s.store.RoomDevices(id))
		return
	}
	if len(p) == 4 && p[3] == "close" {
		if e := a.s.store.CloseRoom(id); e != nil {
			http.Error(w, e.Error(), 400)
			return
		}
		a.json(w, map[string]bool{"ok": true})
		return
	}
	http.NotFound(w, r)
}
func (a *apiServer) devices(w http.ResponseWriter, r *http.Request) {
	if !a.auth(w, r) {
		return
	}
	out := a.s.store.ListDevices()
	log.Printf("debug api devices count=%d", len(out))
	a.s.mu.Lock()
	for i := range out {
		out[i].Online = a.s.peers[out[i].ID] != nil
	}
	a.s.mu.Unlock()
	a.json(w, out)
}
func (a *apiServer) handler(w http.ResponseWriter, r *http.Request) {
	log.Printf("debug http request method=%s path=%s remote=%s", r.Method, r.URL.Path, r.RemoteAddr)
	log.Printf("debug http request method=%s path=%s remote=%s", r.Method, r.URL.Path, r.RemoteAddr)
	switch {
	case r.URL.Path == "/api/login":
		a.login(w, r)
	case r.URL.Path == "/api/rooms":
		a.rooms(w, r)
	case strings.HasPrefix(r.URL.Path, "/api/rooms/"):
		a.roomAction(w, r)
	case r.URL.Path == "/api/devices":
		a.devices(w, r)
	default:
		http.NotFound(w, r)
	}
}
