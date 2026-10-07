//! Rust DSP kernels for aec-web — optional accelerator for `web/dsp.py`.
//!
//! Every function mirrors its Python counterpart bit-for-bit in
//! *framing semantics*; floating-point FFT paths may differ in the
//! last ulp vs pocketfft (different radix code), so parity is
//! asserted with tolerance, plus exact int16 on the smoke pair.
//!
//! Conventions (must match `web/dsp.py`):
//! - 512-sample block, 257-bin spectrum, forward FFT unnormalized,
//!   inverse followed by 1/512 (numpy's `irfft` 1/N).
//! - int16 <-> float via / 32768.0; trunc path truncates toward
//!   zero, round path is round-half-even (`lrintf`).
//! - FIR resample taps computed with the identical windowed-sinc
//!   formula, convolved in float64 like numpy.

use numpy::{PyArray1, PyReadonlyArray1};
use pyo3::prelude::*;
use rustfft::num_complex::Complex32;
use rustfft::{Fft, FftPlanner};
use std::cell::RefCell;
use std::sync::{Arc, OnceLock};

const BLOCK_LEN: usize = 512;
const BINS: usize = 257;
const SAMPLE_RATE: u32 = 16000;

// The audio pump is a single thread; thread-local planners avoid
// rebuilding twiddle tables per FFT without any locking.
thread_local! {
    static FWD: RefCell<Option<Arc<dyn Fft<f32>>>> = const { RefCell::new(None) };
    static INV: RefCell<Option<Arc<dyn Fft<f32>>>> = const { RefCell::new(None) };
}

fn fwd_plan() -> Arc<dyn Fft<f32>> {
    FWD.with(|c| {
        let mut slot = c.borrow_mut();
        if slot.is_none() {
            *slot = Some(FftPlanner::<f32>::new().plan_fft_forward(BLOCK_LEN));
        }
        slot.as_ref().unwrap().clone()
    })
}

fn inv_plan() -> Arc<dyn Fft<f32>> {
    INV.with(|c| {
        let mut slot = c.borrow_mut();
        if slot.is_none() {
            *slot = Some(FftPlanner::<f32>::new().plan_fft_inverse(BLOCK_LEN));
        }
        slot.as_ref().unwrap().clone()
    })
}

/// int16 -> float32, mirrors `micF[i] = mic[i] / 32768.0f`.
#[pyfunction]
fn i16_to_f32<'py>(
    py: Python<'py>,
    x: PyReadonlyArray1<'py, i16>,
) -> Bound<'py, PyArray1<f32>> {
    let s = x.as_slice().expect("contiguous int16 input");
    let out: Vec<f32> = s.iter().map(|&v| v as f32 / 32768.0).collect();
    PyArray1::from_vec(py, out)
}

/// float -> int16 with truncation, mirrors `(int16_t)v` after clip.
#[pyfunction]
fn f32_to_i16_trunc<'py>(
    py: Python<'py>,
    x: PyReadonlyArray1<'py, f32>,
) -> Bound<'py, PyArray1<i16>> {
    let s = x.as_slice().expect("contiguous float32 input");
    let out: Vec<i16> = s
        .iter()
        .map(|&v| (v * 32768.0).clamp(-32768.0, 32767.0) as i16)
        .collect();
    PyArray1::from_vec(py, out)
}

/// float -> int16 with round-half-even, mirrors `clamp_s16(lrintf)`.
#[pyfunction]
fn f32_to_i16_round<'py>(
    py: Python<'py>,
    x: PyReadonlyArray1<'py, f32>,
) -> Bound<'py, PyArray1<i16>> {
    let s = x.as_slice().expect("contiguous float32 input");
    let out: Vec<i16> = s
        .iter()
        .map(|&v| (v * 32768.0).round_ties_even().clamp(-32768.0, 32767.0) as i16)
        .collect();
    PyArray1::from_vec(py, out)
}

/// RMS of a float frame, mirrors `sqrt(dot(v, v) / n)`.
#[pyfunction]
fn rms(x: PyReadonlyArray1<f32>) -> f32 {
    let s = x.as_slice().expect("contiguous float32 input");
    if s.is_empty() {
        return 0.0;
    }
    let sum: f32 = s.iter().map(|&v| v * v).sum();
    (sum / s.len() as f32).sqrt()
}

/// rFFT of a 512-sample block -> (complex spectrum, magnitudes).
#[pyfunction]
fn rfft_mag<'py>(
    py: Python<'py>,
    block: PyReadonlyArray1<'py, f32>,
) -> PyResult<(Bound<'py, PyArray1<Complex32>>, Bound<'py, PyArray1<f32>>)> {
    let b = block.as_slice().expect("contiguous float32 input");
    assert!(b.len() == BLOCK_LEN, "rfft_mag needs 512 samples");
    let mut buf: Vec<Complex32> = b.iter().map(|&v| Complex32::new(v, 0.0)).collect();
    fwd_plan().process(&mut buf);
    // Complex FFT of real input is conjugate-symmetric: bins [0, 257)
    // are the non-redundant half numpy's `rfft` returns.
    buf.truncate(BINS);
    let mag: Vec<f32> = buf.iter().map(|c| c.norm()).collect();
    Ok((
        PyArray1::from_vec(py, buf),
        PyArray1::from_vec(py, mag),
    ))
}

/// irfft(spec * mask) with the 1/N baked in, like numpy's `irfft`.
#[pyfunction]
fn apply_mask_irfft<'py>(
    py: Python<'py>,
    re: PyReadonlyArray1<'py, f32>,
    im: PyReadonlyArray1<'py, f32>,
    mask: PyReadonlyArray1<'py, f32>,
) -> PyResult<Bound<'py, PyArray1<f32>>> {
    let re = re.as_slice().expect("contiguous input");
    let im = im.as_slice().expect("contiguous input");
    let mask = mask.as_slice().expect("contiguous input");
    assert!(re.len() == BINS && im.len() == BINS && mask.len() == BINS);
    let half: Vec<Complex32> = re
        .iter()
        .zip(im.iter())
        .zip(mask.iter())
        .map(|((&r, &i), &m)| Complex32::new(r * m, i * m))
        .collect();
    // Rebuild the full 512 spectrum by conjugate symmetry, exactly
    // what numpy's `irfft` does internally before the inverse pass.
    let mut buf = vec![Complex32::new(0.0, 0.0); BLOCK_LEN];
    buf[..BINS].copy_from_slice(&half);
    for i in BINS..BLOCK_LEN {
        buf[i] = half[BLOCK_LEN - i].conj();
    }
    inv_plan().process(&mut buf);
    // rustfft inverse is unnormalized: divide by N like numpy does.
    let out: Vec<f32> = buf.iter().map(|c| c.re / BLOCK_LEN as f32).collect();
    Ok(PyArray1::from_vec(py, out))
}

// ---- FIR resample helpers (float64 throughout, like numpy) ----

fn build_fir() -> (Vec<f64>, Vec<f64>) {
    let taps: usize = 61;
    let half = (taps / 2) as f64; // integer division, like `taps // 2`
    let fc = 7000.0 / 48000.0;
    let h: Vec<f64> = (0..taps)
        .map(|k| {
            let n = k as f64 - half;
            // np.sinc(x) = sin(pi*x)/(pi*x), sinc(0) = 1.
            let sinc = if n == 0.0 {
                1.0
            } else {
                (std::f64::consts::PI * 2.0 * fc * n).sin()
                    / (std::f64::consts::PI * 2.0 * fc * n)
            };
            let w = 0.54 - 0.46 * (2.0 * std::f64::consts::PI * (n + half)
                / (taps as f64 - 1.0))
                .cos();
            sinc * w
        })
        .collect();
    let sum: f64 = h.iter().sum();
    let normed: Vec<f64> = h.iter().map(|&v| v / sum).collect();
    let up: Vec<f64> = normed.iter().map(|&v| v * 3.0).collect();
    (normed, up)
}

fn fir_taps() -> &'static (Vec<f64>, Vec<f64>) {
    static TAPS: OnceLock<(Vec<f64>, Vec<f64>)> = OnceLock::new();
    TAPS.get_or_init(build_fir)
}

/// Full convolution, then numpy `mode="same"` centering.
fn convolve_same(x: &[f64], h: &[f64]) -> Vec<f64> {
    let n = x.len();
    let m = h.len();
    let mut full = vec![0.0f64; n + m - 1];
    for (i, &xv) in x.iter().enumerate() {
        if xv == 0.0 {
            continue;
        }
        for (j, &hv) in h.iter().enumerate() {
            full[i + j] += xv * hv;
        }
    }
    let start = (m - 1) / 2;
    full[start..start + n].to_vec()
}

fn linspace(a: f64, b: f64, n: usize) -> Vec<f64> {
    if n == 1 {
        return vec![a];
    }
    (0..n).map(|i| a + (b - a) * i as f64 / (n - 1) as f64).collect()
}

fn interp(x: &[f64], xp: &[f64], fp: &[f64]) -> Vec<f64> {
    x.iter()
        .map(|&xi| {
            if xi <= xp[0] {
                return fp[0];
            }
            if xi >= xp[xp.len() - 1] {
                return fp[fp.len() - 1];
            }
            let mut lo = 0usize;
            while xp[lo + 1] < xi {
                lo += 1;
            }
            let t = (xi - xp[lo]) / (xp[lo + 1] - xp[lo]);
            fp[lo] + t * (fp[lo + 1] - fp[lo])
        })
        .collect()
}

/// 48000 -> 16000: FIR lowpass + every 3rd (mirrors `_fir_decimate_3`).
#[pyfunction]
fn fir_decimate_3<'py>(
    py: Python<'py>,
    x: PyReadonlyArray1<'py, f32>,
) -> Bound<'py, PyArray1<f32>> {
    let s = x.as_slice().expect("contiguous input");
    let xd: Vec<f64> = s.iter().map(|&v| v as f64).collect();
    let y = convolve_same(&xd, &fir_taps().0);
    let out: Vec<f32> = y.iter().step_by(3).map(|&v| v as f32).collect();
    PyArray1::from_vec(py, out)
}

/// 16000 -> 48000: zero-stuff x3 + lowpass gain x3 (mirrors `_fir_interpolate_3`).
#[pyfunction]
fn fir_interpolate_3<'py>(
    py: Python<'py>,
    x: PyReadonlyArray1<'py, f32>,
) -> Bound<'py, PyArray1<f32>> {
    let s = x.as_slice().expect("contiguous input");
    let mut up = vec![0.0f64; s.len() * 3];
    for (i, &v) in s.iter().enumerate() {
        up[i * 3] = v as f64;
    }
    let y = convolve_same(&up, &fir_taps().1);
    let out: Vec<f32> = y.iter().map(|&v| v as f32).collect();
    PyArray1::from_vec(py, out)
}

/// Generic linear-interpolation resample 16k -> rate (mirrors `resample_from_16k`).
#[pyfunction]
fn resample_linear_up<'py>(
    py: Python<'py>,
    x: PyReadonlyArray1<'py, f32>,
    rate: u32,
) -> Bound<'py, PyArray1<f32>> {
    let s = x.as_slice().expect("contiguous input");
    if rate == SAMPLE_RATE || s.is_empty() {
        return PyArray1::from_vec(py, s.to_vec());
    }
    let want = ((s.len() as f64 * rate as f64 / SAMPLE_RATE as f64).round()) as usize;
    if want == 0 {
        return PyArray1::from_vec(py, Vec::<f32>::new());
    }
    let xd: Vec<f64> = s.iter().map(|&v| v as f64).collect();
    let y = interp(&linspace(0.0, 1.0, want), &linspace(0.0, 1.0, xd.len()), &xd);
    PyArray1::from_vec(py, y.iter().map(|&v| v as f32).collect())
}

/// Generic linear-interpolation resample rate -> 16k (mirrors `resample_to_16k`).
#[pyfunction]
fn resample_linear_down<'py>(
    py: Python<'py>,
    x: PyReadonlyArray1<'py, f32>,
    rate: u32,
) -> Bound<'py, PyArray1<f32>> {
    let s = x.as_slice().expect("contiguous input");
    if rate == SAMPLE_RATE || s.is_empty() {
        return PyArray1::from_vec(py, s.to_vec());
    }
    let want = ((s.len() as f64 * SAMPLE_RATE as f64 / rate as f64).round()) as usize;
    if want == 0 {
        return PyArray1::from_vec(py, Vec::<f32>::new());
    }
    let xd: Vec<f64> = s.iter().map(|&v| v as f64).collect();
    let y = interp(&linspace(0.0, 1.0, want), &linspace(0.0, 1.0, xd.len()), &xd);
    PyArray1::from_vec(py, y.iter().map(|&v| v as f32).collect())
}

#[pymodule]
fn aec_dsp(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(i16_to_f32, m)?)?;
    m.add_function(wrap_pyfunction!(f32_to_i16_trunc, m)?)?;
    m.add_function(wrap_pyfunction!(f32_to_i16_round, m)?)?;
    m.add_function(wrap_pyfunction!(rms, m)?)?;
    m.add_function(wrap_pyfunction!(rfft_mag, m)?)?;
    m.add_function(wrap_pyfunction!(apply_mask_irfft, m)?)?;
    m.add_function(wrap_pyfunction!(fir_decimate_3, m)?)?;
    m.add_function(wrap_pyfunction!(fir_interpolate_3, m)?)?;
    m.add_function(wrap_pyfunction!(resample_linear_up, m)?)?;
    m.add_function(wrap_pyfunction!(resample_linear_down, m)?)?;
    Ok(())
}
