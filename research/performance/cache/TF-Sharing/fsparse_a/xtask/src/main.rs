use std::process::Command;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let cmd = args.first().map(|s| s.as_str()).unwrap_or("build");

    match cmd {
        "build-ebpf" => build_ebpf(),
        "build" => {
            build_ebpf();
            build_userspace();
        }
        "run" => {
            build_ebpf();
            run_userspace(&args[1..]);
        }
        "test" => {
            build_ebpf();
            test_userspace();
        }
        _ => {
            eprintln!("Usage: xtask <build-ebpf|build|run|test>");
            std::process::exit(1);
        }
    }
}

fn build_ebpf() {
    println!(">> Building eBPF program...");
    let status = Command::new("cargo")
        .args([
            "+nightly",
            "build",
            "-p",
            "fsparse_a-ebpf",
            "--target",
            "bpfel-unknown-none",
            "-Z",
            "build-std=core,alloc",
            "--release",
        ])
        .status()
        .expect("failed to run cargo build for eBPF");
    if !status.success() {
        eprintln!("!! eBPF build failed");
        std::process::exit(1);
    }
    println!(">> eBPF program built: target/bpfel-unknown-none/release/fsparse_a-ebpf");
}

fn build_userspace() {
    println!(">> Building user space...");
    let status = Command::new("cargo")
        .args(["build", "-p", "fsparse_a", "--release"])
        .status()
        .expect("failed to run cargo build for user space");
    if !status.success() {
        eprintln!("!! user space build failed");
        std::process::exit(1);
    }
    println!(">> user space built: target/release/fsparse_a");
}

fn run_userspace(extra_args: &[String]) {
    build_ebpf();
    let mut cmd = Command::new("cargo");
    cmd.args(["run", "-p", "fsparse_a", "--release", "--"]);
    cmd.args(extra_args);
    cmd.status().expect("failed to run user space");
}

fn test_userspace() {
    let status = Command::new("cargo")
        .args(["test", "--workspace"])
        .status()
        .expect("failed to run cargo test");
    if !status.success() {
        std::process::exit(1);
    }
}
