// Example: using liblocalvqe.so from Go with purego.
//
// This file demonstrates the purego FFI bindings for VoxInput / LocalAI.
// It is a standalone test file — not part of the build.
//
// Usage:
//   go test -v -run TestLocalVQE ./ggml/
//
// Requires liblocalvqe.so in LD_LIBRARY_PATH or the build directory.

//go:build ignore

package localvqe_test

import (
	"fmt"
	"unsafe"

	"github.com/ebitengine/purego"
)

// LocalVQE wraps the C API via purego (no cgo required).
type LocalVQE struct {
	lib uintptr
	ctx uintptr

	fnFree       func(uintptr)
	fnProcessF32 func(uintptr, uintptr, uintptr, int32, uintptr) int32
	fnProcessS16 func(uintptr, uintptr, uintptr, int32, uintptr) int32
	fnLastError  func(uintptr) uintptr
	fnSampleRate func(uintptr) int32
	fnHopLength  func(uintptr) int32
	fnFFTSize    func(uintptr) int32
}

// NewLocalVQE loads the shared library and model.
func NewLocalVQE(libPath, modelPath string) (*LocalVQE, error) {
	lib, err := purego.Dlopen(libPath, purego.RTLD_LAZY)
	if err != nil {
		return nil, fmt.Errorf("dlopen: %w", err)
	}

	d := &LocalVQE{lib: lib}

	// Register all functions
	var fnNew func(uintptr) uintptr
	purego.RegisterLibFunc(&fnNew, lib, "localvqe_new")
	purego.RegisterLibFunc(&d.fnFree, lib, "localvqe_free")
	purego.RegisterLibFunc(&d.fnProcessF32, lib, "localvqe_process_f32")
	purego.RegisterLibFunc(&d.fnProcessS16, lib, "localvqe_process_s16")
	purego.RegisterLibFunc(&d.fnLastError, lib, "localvqe_last_error")
	purego.RegisterLibFunc(&d.fnSampleRate, lib, "localvqe_sample_rate")
	purego.RegisterLibFunc(&d.fnHopLength, lib, "localvqe_hop_length")
	purego.RegisterLibFunc(&d.fnFFTSize, lib, "localvqe_fft_size")

	// Load model (pass string as *byte)
	pathBytes := append([]byte(modelPath), 0) // null-terminated
	d.ctx = fnNew(uintptr(unsafe.Pointer(&pathBytes[0])))
	if d.ctx == 0 {
		purego.Dlclose(lib)
		return nil, fmt.Errorf("localvqe_new failed for %s", modelPath)
	}

	return d, nil
}

func (d *LocalVQE) Close() {
	if d.ctx != 0 {
		d.fnFree(d.ctx)
		d.ctx = 0
	}
	if d.lib != 0 {
		purego.Dlclose(d.lib)
		d.lib = 0
	}
}

// ProcessF32 runs AEC on float32 audio buffers.
// mic and ref must be the same length, 16kHz mono, range [-1,1].
func (d *LocalVQE) ProcessF32(mic, ref []float32) ([]float32, error) {
	n := int32(len(mic))
	out := make([]float32, n)
	ret := d.fnProcessF32(
		d.ctx,
		uintptr(unsafe.Pointer(&mic[0])),
		uintptr(unsafe.Pointer(&ref[0])),
		n,
		uintptr(unsafe.Pointer(&out[0])),
	)
	if ret != 0 {
		return nil, fmt.Errorf("localvqe_process_f32 error %d: %s", ret, d.LastError())
	}
	return out, nil
}

// ProcessS16 runs AEC on int16 PCM buffers (16kHz mono).
func (d *LocalVQE) ProcessS16(mic, ref []int16) ([]int16, error) {
	n := int32(len(mic))
	out := make([]int16, n)
	ret := d.fnProcessS16(
		d.ctx,
		uintptr(unsafe.Pointer(&mic[0])),
		uintptr(unsafe.Pointer(&ref[0])),
		n,
		uintptr(unsafe.Pointer(&out[0])),
	)
	if ret != 0 {
		return nil, fmt.Errorf("localvqe_process_s16 error %d: %s", ret, d.LastError())
	}
	return out, nil
}

func (d *LocalVQE) LastError() string {
	ptr := d.fnLastError(d.ctx)
	if ptr == 0 {
		return ""
	}
	// Read null-terminated C string
	var buf []byte
	for i := uintptr(0); ; i++ {
		b := *(*byte)(unsafe.Pointer(ptr + i))
		if b == 0 {
			break
		}
		buf = append(buf, b)
	}
	return string(buf)
}

func (d *LocalVQE) SampleRate() int { return int(d.fnSampleRate(d.ctx)) }
func (d *LocalVQE) HopLength() int  { return int(d.fnHopLength(d.ctx)) }
func (d *LocalVQE) FFTSize() int    { return int(d.fnFFTSize(d.ctx)) }
