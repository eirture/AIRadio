package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"time"

	"github.com/hraban/opus"
)

const wavSampleRate = 16000

type wavRecording struct {
	file     *os.File
	decoder  *opus.Decoder
	path     string
	pcmBytes uint32
}

func (s *server) beginRecording(p *peer) error {
	decoder, err := opus.NewDecoder(wavSampleRate, 1)
	if err != nil {
		return err
	}
	name := fmt.Sprintf("%s-%s-*.wav.part", p.id, time.Now().UTC().Format("20060102T150405Z"))
	file, err := os.CreateTemp(s.recordDir, name)
	if err != nil {
		return err
	}
	recording := &wavRecording{file: file, decoder: decoder, path: file.Name()[:len(file.Name())-len(".part")]}
	if _, err := file.Write(make([]byte, 44)); err != nil {
		recording.discard()
		return err
	}
	p.recording = recording
	return nil
}

func (r *wavRecording) appendOpus(packet []byte) error {
	pcm := make([]int16, 960)
	n, err := r.decoder.Decode(packet, pcm)
	if err != nil {
		return err
	}
	if n <= 0 || n > len(pcm) || r.pcmBytes+uint32(n*2) > 960000 {
		return errors.New("invalid decoded frame length")
	}
	if err := binary.Write(r.file, binary.LittleEndian, pcm[:n]); err != nil {
		return err
	}
	r.pcmBytes += uint32(n * 2)
	return nil
}

func (r *wavRecording) finish() error {
	if r.pcmBytes == 0 {
		return errors.New("empty recording")
	}
	if _, err := r.file.Seek(0, io.SeekStart); err != nil {
		return err
	}
	header := []byte{
		'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
		'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0,
		'd', 'a', 't', 'a', 0, 0, 0, 0,
	}
	binary.LittleEndian.PutUint32(header[4:8], r.pcmBytes+36)
	binary.LittleEndian.PutUint32(header[24:28], wavSampleRate)
	binary.LittleEndian.PutUint32(header[28:32], wavSampleRate*2)
	binary.LittleEndian.PutUint32(header[40:44], r.pcmBytes)
	if _, err := r.file.Write(header); err != nil {
		return err
	}
	if err := r.file.Sync(); err != nil {
		return err
	}
	if err := r.file.Close(); err != nil {
		return err
	}
	return os.Rename(r.file.Name(), r.path)
}

func (r *wavRecording) discard() {
	if r.file != nil {
		r.file.Close()
		os.Remove(r.file.Name())
	}
}

func (s *server) discardRecording(p *peer) {
	if p.recording != nil {
		p.recording.discard()
		p.recording = nil
	}
}
