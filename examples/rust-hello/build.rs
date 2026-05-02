// Tells the linker where libhal.a lives and to link against it.
//
// We don't invoke `make -C ../..` from build.rs: the top-level Makefile
// shim handles ordering (libhal.a then `cargo build`) and that keeps
// nix-develop activations explicit. If libhal.a is missing the linker
// will say so loudly enough.
fn main() {
    let hal = "../..";
    println!("cargo:rustc-link-search=native={hal}");
    println!("cargo:rustc-link-lib=static=hal");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed={hal}/libhal.a");
    println!("cargo:rerun-if-changed={hal}/link.ld");
}
