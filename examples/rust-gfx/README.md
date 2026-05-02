# rust-gfx

`embedded-graphics` driving the rvvm-hal framebuffer.

Implements `DrawTarget<Color = Rgb888>` over HAL's `gfx_t`, then runs
a small animated scene: title in a mono bitmap font, a bordered
backdrop, four bouncing balls on Lissajous paths, and a frame counter
HUD — all from `embedded-graphics` primitives, no plotting by hand.
60 fps wfi-paced for ~8 seconds, then `hal_exit(0)`.

## Why embedded-graphics

It's the canonical `no_std`/`no_alloc` 2D graphics crate for Rust, and
fits the HAL surface naturally:

- `DrawTarget` trait → wrap the HAL's `vram` slice
- One `fill_solid` override turns clear-screen + solid rects into
  direct row fills (~100× faster than the default per-pixel path)
- Fonts (`FONT_10X20`, `FONT_6X10`), shapes (`Circle`, `Rectangle`),
  text alignment, primitive styles — all available, all const-friendly
- ~14 KiB extra in the binary over rust-hello

## Build

```sh
nix develop ../.. --command make
```

Produces `firmware.bin` (~22 KiB).

## Run

```sh
make run            # RVVM with Bochs Display (mode-set to 800×480)
make run-headless   # RVVM with simple-framebuffer at 800×480, no GUI
make run-qemu       # QEMU virt
```

Headless still exercises the full draw path — UART output confirms
60 fps end-to-end even without a window:

```
rust-gfx: bringing up framebuffer...
gfx: simple-framebuffer @ 0x0000000018000000  800x480  stride=800 px  fmt=a8r8g8b8
rust-gfx: 800x480, backend=2, format=0
rust-gfx: drew 481 frames; bye.

!!HAL-EXIT code=0
```

## How it works

The `Framebuffer` shim is the entire HAL-↔-embedded-graphics bridge:

```rust
impl OriginDimensions for Framebuffer { /* size() from gfx_t */ }
impl DrawTarget for Framebuffer {
    type Color = Rgb888;
    type Error = core::convert::Infallible;
    fn draw_iter(...)   { /* per-pixel encode + bounds check */ }
    fn fill_solid(...)  { /* row-fill fast path */ }
}
```

Channel order is handled in `encode()` based on `gfx_t.format` — the
same firmware works on both XRGB8888 (Bochs) and XBGR8888 (some
simplefb hosts) without recompilation.

Once `DrawTarget` is implemented, every embedded-graphics consumer
crate works unchanged on RVVM — `tinybmp`, `tinytga`, `u8g2-fonts`,
`embedded-canvas`, `eg-seven-segment`, etc. The HAL doesn't need to
know about any of them.

## Double-buffered

The demo opts into HAL page-flipping via `gfx_enable_double_buffer`
when the backend supports it (Bochs only — simple-framebuffer has no
offset register). On Bochs that doubles `VIRT_HEIGHT`; the demo
draws into the off-screen half and `gfx_flip` swaps which half is
on-screen with one `Y_OFFSET` register write. Result: the host display
only ever sees whole frames, no mid-render tearing.

When DB succeeds the UART logs `rust-gfx: double-buffer = ON`; the
draw loop then calls `gfx_flip` per frame and re-threads the new
back-buffer pointer into the `Framebuffer` shim. When DB is refused
(simplefb), the demo falls back to `gfx_present_all` and runs unchanged.

## Caveats

- `time_now()` from `time.h` is `static inline` (a `rdtime` CSR
  read), so it has no symbol in `libhal.a`. We replicate it as one
  line of inline asm in Rust. Same for `gfx_present_all`, which we
  inline as a call to the non-inline `gfx_present`. Anything else
  `static inline` in HAL headers needs the same treatment.
- The Lissajous "physics" is two sin LUT lookups per ball — no real
  collision, no float. Swap in a proper integrator if you want actual
  bouncing dynamics.
