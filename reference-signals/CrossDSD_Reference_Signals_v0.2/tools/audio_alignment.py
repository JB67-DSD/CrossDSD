"""Shared integer and fractional alignment helpers for CrossDSD measurements."""

from __future__ import annotations

import numpy as np


def parabolic_peak_position(samples: np.ndarray, channel: int = 0) -> float:
    """Estimate an impulse peak position to sub-sample precision."""
    magnitude = np.abs(samples[:, channel])
    index = int(np.argmax(magnitude))
    if index == 0 or index == len(magnitude) - 1:
        return float(index)
    left, center, right = magnitude[index - 1 : index + 2]
    denominator = left - 2.0 * center + right
    offset = 0.0 if denominator == 0 else 0.5 * (left - right) / denominator
    return index + float(np.clip(offset, -0.5, 0.5))


def fractional_lag(master: np.ndarray, decoded: np.ndarray, channel: int = 0) -> float:
    """Return decoded-minus-master impulse lag in samples."""
    return parabolic_peak_position(decoded, channel) - parabolic_peak_position(master, channel)


def align_fractional(
    master: np.ndarray, decoded: np.ndarray, lag: float
) -> tuple[np.ndarray, np.ndarray]:
    """Align decoded PCM with a cubic Lagrange fractional-sample advance."""
    integer = int(np.floor(lag))
    fraction = float(lag - integer)
    master_start = max(-integer, 0)
    decoded_start = max(integer, 0)

    # One sample before and two samples after are required by the cubic kernel.
    if decoded_start < 1:
        shift = 1 - decoded_start
        decoded_start += shift
        master_start += shift
    length = min(
        len(master) - master_start,
        len(decoded) - decoded_start - 2,
    )
    if length <= 0:
        raise ValueError("No overlapping samples after fractional alignment")

    base = decoded[decoded_start - 1 : decoded_start + length + 2]
    f = fraction
    c_minus_1 = -f * (f - 1.0) * (f - 2.0) / 6.0
    c_zero = (f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0
    c_plus_1 = -(f + 1.0) * f * (f - 2.0) / 2.0
    c_plus_2 = (f + 1.0) * f * (f - 1.0) / 6.0
    shifted = (
        c_minus_1 * base[0:length]
        + c_zero * base[1 : length + 1]
        + c_plus_1 * base[2 : length + 2]
        + c_plus_2 * base[3 : length + 3]
    )
    return master[master_start : master_start + length], shifted

