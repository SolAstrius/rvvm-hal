// rust-gfx — embedded-graphics demo on the rvvm-hal framebuffer.
//
// Implements DrawTarget over HAL's gfx_t, then drives it with a small
// scene: a title in a mono bitmap font, four bouncing circles with
// individual hues, a bordered rectangle, and a frame counter — all
// from embedded-graphics primitives, no plotting by hand.
//
// 60-fps wfi-paced for ~8 seconds, then exits cleanly via hal_exit.

#![no_std]
#![no_main]

use core::ffi::{c_char, c_int};
use core::panic::PanicInfo;
use core::slice;

use embedded_graphics::{
    mono_font::{ascii::{FONT_10X20, FONT_6X10}, MonoTextStyle},
    pixelcolor::Rgb888,
    prelude::*,
    primitives::{Circle, PrimitiveStyle, PrimitiveStyleBuilder, Rectangle},
    text::{Alignment, Text},
};

// ---------- HAL surface ----------

#[repr(C)]
struct Fdt {
    struct_blob: *const u8,
    strings: *const c_char,
    struct_size: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct Gfx {
    vram: *mut u32,
    width: u32,
    height: u32,
    stride_px: u32,
    backend: i32,
    format: i32, // 0 = XRGB8888, 1 = XBGR8888
}

unsafe extern "C" {
    fn uart_init(base: usize);
    fn uart_puts(s: *const c_char);
    fn uart_printf(fmt: *const c_char, ...);
    fn hal_panic(fmt: *const c_char, ...) -> !;
    fn hal_exit(code: c_int) -> !;

    fn fdt_init(fdt: *mut Fdt, blob: *const u8) -> bool;
    fn gfx_init_fdt(g: *mut Gfx, fdt: *const Fdt, want_w: u32, want_h: u32) -> bool;
    fn gfx_present(g: *const Gfx, x: u32, y: u32, w: u32, h: u32);
    fn gfx_enable_double_buffer(g: *mut Gfx) -> bool;
    fn gfx_flip(g: *mut Gfx);

    fn time_hz() -> u64;
    fn time_ticks_per_frame() -> u64;
    fn time_busy_until(deadline: u64);
}

// `time_now()` is `static inline` in time.h (just a `rdtime` CSR
// read), so it has no symbol in libhal.a. Replicate it here.
#[inline(always)]
fn time_now() -> u64 {
    let v: u64;
    unsafe { core::arch::asm!("rdtime {0}", out(reg) v, options(nomem, nostack, preserves_flags)) };
    v
}

#[inline(always)]
unsafe fn gfx_present_all(g: *const Gfx) {
    unsafe { gfx_present(g, 0, 0, (*g).width, (*g).height) };
}

// ---------- DrawTarget over HAL framebuffer ----------

/// Thin DrawTarget shim. Holds a copy of the Gfx struct (it's just
/// pointer + dims, no ownership semantics) and a precomputed
/// channel-swap flag for XBGR8888 backends.
struct Framebuffer {
    vram: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    swap_rb: bool,
}

impl Framebuffer {
    fn new(g: &Gfx) -> Self {
        Self {
            vram: g.vram,
            width: g.width,
            height: g.height,
            stride: g.stride_px,
            swap_rb: g.format == 1,
        }
    }

    /// Encode a Rgb888 to one u32 in the framebuffer's native order.
    /// XRGB8888 (a8r8g8b8 in memory): u32 = 0x00RRGGBB
    /// XBGR8888 (a8b8g8r8 in memory): u32 = 0x00BBGGRR
    #[inline(always)]
    fn encode(&self, c: Rgb888) -> u32 {
        let r = c.r() as u32;
        let g = c.g() as u32;
        let b = c.b() as u32;
        if self.swap_rb {
            (b << 16) | (g << 8) | r
        } else {
            (r << 16) | (g << 8) | b
        }
    }

    fn buf(&self) -> &mut [u32] {
        unsafe { slice::from_raw_parts_mut(self.vram, (self.stride * self.height) as usize) }
    }
}

impl OriginDimensions for Framebuffer {
    fn size(&self) -> Size {
        Size::new(self.width, self.height)
    }
}

impl DrawTarget for Framebuffer {
    type Color = Rgb888;
    type Error = core::convert::Infallible;

    fn draw_iter<I>(&mut self, pixels: I) -> Result<(), Self::Error>
    where
        I: IntoIterator<Item = Pixel<Rgb888>>,
    {
        let w = self.width as i32;
        let h = self.height as i32;
        let stride = self.stride as usize;
        let buf = self.buf();
        for Pixel(Point { x, y }, color) in pixels {
            if x >= 0 && x < w && y >= 0 && y < h {
                buf[y as usize * stride + x as usize] = self.encode(color);
            }
        }
        Ok(())
    }

    /// Override the fill_solid hot path — embedded-graphics' default
    /// would emit one Pixel per cell. Direct row-fill is ~100× faster
    /// for clear-screen + large-rect cases.
    fn fill_solid(&mut self, area: &Rectangle, color: Rgb888) -> Result<(), Self::Error> {
        let area = area.intersection(&self.bounding_box());
        if area.size.width == 0 || area.size.height == 0 {
            return Ok(());
        }
        let val = self.encode(color);
        let stride = self.stride as usize;
        let buf = self.buf();
        let x0 = area.top_left.x as usize;
        let y0 = area.top_left.y as usize;
        let w = area.size.width as usize;
        let h = area.size.height as usize;
        for y in y0..y0 + h {
            let row = &mut buf[y * stride + x0..y * stride + x0 + w];
            row.fill(val);
        }
        Ok(())
    }
}

// ---------- Sin LUT for bouncing motion ----------

const SIN_LUT: [i16; 256] = {
    let mut out = [0i16; 256];
    let mut i = 0;
    while i < 256 {
        // Bhaskara I sine, scaled to [-1000, 1000]. Integer-only so
        // it lives in const.
        let deg = (i as i32 * 360) / 256;
        let (theta, sign) = if deg <= 180 { (deg, 1i32) } else { (deg - 180, -1i32) };
        let num = 4 * theta * (180 - theta);
        let den = 40500 - theta * (180 - theta);
        out[i] = (sign * num * 1000 / den) as i16;
        i += 1;
    }
    out
};

#[inline(always)]
fn sin_q(x: u32) -> i32 {
    SIN_LUT[(x & 0xFF) as usize] as i32
}

// ---------- Scene ----------

#[derive(Copy, Clone)]
struct Ball {
    color: Rgb888,
    radius: u32,
    // Phase offsets and amplitudes for x/y sinusoids — just two sines
    // per ball gives Lissajous-curve bouncing without needing a real
    // physics step.
    phase_x: u32,
    phase_y: u32,
    speed_x: u32,
    speed_y: u32,
}

const BALLS: &[Ball] = &[
    Ball { color: Rgb888::new(0xff, 0x55, 0x88), radius: 18, phase_x:   0, phase_y:  64, speed_x: 2, speed_y: 3 },
    Ball { color: Rgb888::new(0x55, 0xff, 0xaa), radius: 14, phase_x:  96, phase_y: 160, speed_x: 3, speed_y: 2 },
    Ball { color: Rgb888::new(0x55, 0xaa, 0xff), radius: 22, phase_x: 200, phase_y:   0, speed_x: 1, speed_y: 4 },
    Ball { color: Rgb888::new(0xff, 0xcc, 0x44), radius: 12, phase_x:  20, phase_y: 220, speed_x: 4, speed_y: 1 },
];

fn draw_scene(fb: &mut Framebuffer, frame: u32) {
    // Background: a horizontal band gradient via two filled rects.
    // Cheap and gives the bouncing balls something to pop against.
    let w = fb.size().width;
    let h = fb.size().height;
    fb.fill_solid(&fb.bounding_box(), Rgb888::new(0x10, 0x12, 0x1c)).unwrap();
    fb.fill_solid(
        &Rectangle::new(Point::new(0, (h as i32) - 80), Size::new(w, 80)),
        Rgb888::new(0x18, 0x1c, 0x2a),
    ).unwrap();

    // Decorative border.
    Rectangle::new(Point::new(8, 8), Size::new(w - 16, h - 16))
        .into_styled(PrimitiveStyle::with_stroke(Rgb888::new(0x44, 0x55, 0x88), 2))
        .draw(fb)
        .unwrap();

    // Title.
    let title_style = MonoTextStyle::new(&FONT_10X20, Rgb888::new(0xee, 0xee, 0xff));
    Text::with_alignment(
        "rust + rvvm-hal",
        Point::new((w as i32) / 2, 36),
        title_style,
        Alignment::Center,
    )
    .draw(fb)
    .unwrap();

    // Subtitle: animate the colour by walking the sin LUT — no per-
    // frame allocation, and `?:`-style branching stays trivial in
    // codegen.
    let pulse = ((sin_q(frame) + 1000) / 8) as u8; // 0..250
    let sub_style = MonoTextStyle::new(
        &FONT_6X10,
        Rgb888::new(0x88 + (pulse / 2), 0x99, 0xff - (pulse / 2)),
    );
    Text::with_alignment(
        "embedded-graphics over a HAL framebuffer",
        Point::new((w as i32) / 2, 56),
        sub_style,
        Alignment::Center,
    )
    .draw(fb)
    .unwrap();

    // Bouncing balls. Each ball's centre is `centre + sin*amp` along
    // both axes — bounded so they stay inside the bordered region.
    let pad = 32i32;
    let cx0 = pad + 30;
    let cx1 = (w as i32) - pad - 30;
    let cy0 = 80;
    let cy1 = (h as i32) - pad - 16;
    let amp_x = (cx1 - cx0) / 2;
    let amp_y = (cy1 - cy0) / 2;
    let mid_x = (cx0 + cx1) / 2;
    let mid_y = (cy0 + cy1) / 2;

    for ball in BALLS {
        let sx = sin_q(ball.phase_x.wrapping_add(frame.wrapping_mul(ball.speed_x)));
        let sy = sin_q(ball.phase_y.wrapping_add(frame.wrapping_mul(ball.speed_y)));
        let cx = mid_x + (sx * amp_x) / 1000;
        let cy = mid_y + (sy * amp_y) / 1000;

        let style = PrimitiveStyleBuilder::new()
            .fill_color(ball.color)
            .stroke_color(Rgb888::new(0xff, 0xff, 0xff))
            .stroke_width(1)
            .build();
        Circle::with_center(Point::new(cx, cy), ball.radius * 2)
            .into_styled(style)
            .draw(fb)
            .unwrap();
    }

    // HUD: frame counter in the bottom-left.
    let hud_style = MonoTextStyle::new(&FONT_6X10, Rgb888::new(0x99, 0xaa, 0xcc));
    let mut buf = [0u8; 32];
    let s = format_frame(&mut buf, frame);
    Text::new(s, Point::new(16, (h as i32) - 12), hud_style)
        .draw(fb)
        .unwrap();
}

/// Tiny u32 → "frame: NNN" formatter into a stack buffer. Avoids
/// pulling core::fmt into the binary just for this; saves a few KiB.
fn format_frame(buf: &mut [u8; 32], n: u32) -> &str {
    let prefix = b"frame ";
    buf[..prefix.len()].copy_from_slice(prefix);
    let mut i = prefix.len();
    if n == 0 {
        buf[i] = b'0';
        i += 1;
    } else {
        let mut digits = [0u8; 10];
        let mut d = 0;
        let mut v = n;
        while v > 0 {
            digits[d] = b'0' + (v % 10) as u8;
            v /= 10;
            d += 1;
        }
        while d > 0 {
            d -= 1;
            buf[i] = digits[d];
            i += 1;
        }
    }
    core::str::from_utf8(&buf[..i]).unwrap()
}

// ---------- Entry ----------

const TARGET_W: u32 = 800;
const TARGET_H: u32 = 480;
const RUN_SECONDS: u64 = 8;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn kmain(_hartid: u64, fdt_addr: u64) -> ! {
    unsafe {
        uart_init(0);
        uart_puts(c"rust-gfx: bringing up framebuffer...\n".as_ptr());

        let mut fdt = Fdt {
            struct_blob: core::ptr::null(),
            strings: core::ptr::null(),
            struct_size: 0,
        };
        if !fdt_init(&mut fdt, fdt_addr as *const u8) {
            hal_panic(c"rust-gfx: fdt_init failed\n".as_ptr());
        }

        let mut g = Gfx {
            vram: core::ptr::null_mut(),
            width: 0,
            height: 0,
            stride_px: 0,
            backend: 0,
            format: 0,
        };
        if !gfx_init_fdt(&mut g, &fdt, TARGET_W, TARGET_H) {
            uart_puts(c"rust-gfx: no framebuffer available, exiting.\n".as_ptr());
            hal_exit(0);
        }
        uart_printf(
            c"rust-gfx: %ux%u, backend=%d, format=%d\n".as_ptr(),
            g.width,
            g.height,
            g.backend,
            g.format,
        );

        // Opt in to page-flipped double buffering. On Bochs the HAL
        // doubles VIRT_HEIGHT and we draw into the off-screen half;
        // gfx_flip() swaps the visible region in one register write,
        // so the host display sees whole frames only — no mid-blit
        // tearing even on a slow render path.
        let db_enabled = gfx_enable_double_buffer(&mut g);
        uart_printf(
            c"rust-gfx: double-buffer = %s\n".as_ptr(),
            if db_enabled { c"ON".as_ptr() } else { c"OFF (backend unsupported)".as_ptr() },
        );

        let mut fb = Framebuffer::new(&g);

        let frame_ticks = time_ticks_per_frame();
        let total_ticks = time_hz() * RUN_SECONDS;
        let start = time_now();
        let deadline_end = start + total_ticks;

        let mut next_frame = start;
        let mut frame: u32 = 0;
        while time_now() < deadline_end {
            draw_scene(&mut fb, frame);
            if db_enabled {
                // gfx_flip swaps which VRAM half the host displays AND
                // updates g.vram to the new back buffer — re-thread
                // it into Framebuffer so the next frame draws there.
                gfx_flip(&mut g);
                fb = Framebuffer::new(&g);
            } else {
                gfx_present_all(&g);
            }
            frame = frame.wrapping_add(1);
            next_frame += frame_ticks;
            time_busy_until(next_frame);
        }

        uart_printf(c"rust-gfx: drew %u frames; bye.\n".as_ptr(), frame);
        hal_exit(0);
    }
}

// ---------- Panic ----------

#[panic_handler]
fn on_panic(info: &PanicInfo) -> ! {
    let loc = info.location();
    unsafe {
        match loc {
            Some(l) => hal_panic(
                c"rust panic at %s:%u\n".as_ptr(),
                l.file().as_ptr(),
                l.line(),
            ),
            None => hal_panic(c"rust panic (no location)\n".as_ptr()),
        }
    }
}
