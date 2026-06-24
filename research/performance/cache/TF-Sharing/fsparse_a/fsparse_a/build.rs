//! build.rs: 在编译用户空间时嵌入 eBPF 字节码
//!
//! 从 ../target/bpfel-unknown-none/release/fsparse_a-ebpf 读取
//! 已编译的 eBPF 程序，并生成 BPF_EBPF_BYTES 常量。

use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let workspace_root = manifest_dir.parent().unwrap();
    let ebpf_path = workspace_root
        .join("target")
        .join("bpfel-unknown-none")
        .join("release")
        .join("fsparse_a-ebpf");

    println!("cargo:rerun-if-changed={}", ebpf_path.display());

    if !ebpf_path.exists() {
        // eBPF 程序尚未构建，生成空字节码占位
        println!("cargo:warning=eBPF program not found at {}, run `cargo xtask build-ebpf` first", ebpf_path.display());
        fs::write(
            PathBuf::from(env::var("OUT_DIR").unwrap()).join("ebpf_bytes.rs"),
            "pub static BPF_EBPF_BYTES: &[u8] = &[];\n",
        )
        .unwrap();
        return;
    }

    let bytes = fs::read(&ebpf_path).expect("failed to read eBPF program");
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let out_path = out_dir.join("ebpf_bytes.rs");

    let mut content = String::from("pub static BPF_EBPF_BYTES: &[u8] = &[\n");
    for byte in &bytes {
        content.push_str(&format!("    0x{:02x},\n", byte));
    }
    content.push_str("];\n");

    fs::write(&out_path, content).unwrap();
    println!(
        "cargo:warning=embedded eBPF program ({} bytes)",
        bytes.len()
    );
}
