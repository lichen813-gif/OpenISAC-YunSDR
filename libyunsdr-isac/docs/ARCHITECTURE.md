# Architecture

## Data path

```text
VLC/UDP
  -> OpenISAC video framing
  -> OFDM/LDPC/MIMO transmitter
  -> libyunsdr_isac::TxStream
  -> YunSDR TX
  -> attenuated cable or controlled over-the-air path
  -> YunSDR RX
  -> libyunsdr_isac::RxStream
  -> OpenISAC synchronization/demodulation/CRC
  -> UDP/VLC
```

## Ownership

OpenISAC continues to own modulation, frame construction, ZC synchronization,
channel estimation, detection, LDPC, CRC, sensing, and telemetry. This project
owns only the hardware boundary:

- device lifecycle and capability discovery;
- sample rate, LO, RF bandwidth, gain, reference clock, and channel selection;
- `std::complex<float>` to/from the vendor wire format;
- single-port and coherent multi-port streaming;
- timestamp conversion and scheduled start/stop;
- short read/write handling and bounded queues;
- timeout, overflow, underflow, alignment, and sequence event mapping;
- hardware smoke tests and RF loopback executables.

## Interface mapping

The real adapter shall implement the contracts in OpenISAC's
`include/RadioBackend.hpp`:

| OpenISAC contract | libyunsdr-isac responsibility |
| --- | --- |
| `radio::IDevice` | Open/close, tune, rate, bandwidth, gain, clock, streams |
| `radio::ITxStream` | IQ conversion, timestamped writes, TX event reporting |
| `radio::IRxStream` | IQ conversion, timestamped reads, RX error reporting |
| `radio::TimeSpec` | Lossless conversion between device ticks and seconds |
| `radio::StreamArgs` | Channel list and CPU/wire sample formats |

Vendor headers must remain inside implementation files or a private PIMPL so
they do not leak into the public OpenISAC interface.

The phase-1 C++ lifecycle and mode interfaces are implemented in
`include/libyunsdr_isac/transceiver.hpp` and `phy_pipeline.hpp`. Detailed mode,
threading, video-flow and fake-test design is documented in
`CPP_HARDWARE_FLOW_INTERFACES.md`.

## Integration stages

1. Import the exact Windows SDK and the analysis handoff package.
2. Build a device probe without any PHY dependency beyond the radio contract.
3. Capture IQ and transmit a bounded known waveform independently.
4. Run a cabled SISO RF loopback with verified attenuation.
5. Connect the continuous OpenISAC SISO frame pipeline and measure EVM/FER.
6. Connect the UDP/VLC video bridge and run a long-duration stability test.
7. Add 2x2 and 4x4 only after channel coherence and timestamp alignment pass.

Never connect a transmitter directly to a receiver. Select attenuation from the
exact hardware maximum input and output specifications before RF loopback.
