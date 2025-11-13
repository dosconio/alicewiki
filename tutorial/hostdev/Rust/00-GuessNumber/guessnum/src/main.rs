// ASCII Rust TAB4 CRLF
// Attribute: 
// LastCheck: 20240410
// AllAuthor: @dosconio
// ModuTitle: Guess Number - Basic IO
// Copyright: 

use std::io; // prelude
use rand::Rng; // trait
use std::cmp::Ordering;
fn main() {
	// - Except 用于处理 Err 和 Ok 的情况
	println!("Hello, world!");

	let secret = rand::thread_rng().gen_range(1,101); // yo [1,101)

	loop {
		println!("Guess the number:");
		let mut guess = String::new();

		io::stdin().read_line(&mut guess).expect("EXC: Failed to read line");
		let guess: u32 = guess.trim().parse().expect("EXC: Please type a number!");

		match guess.cmp(&secret) {
			Ordering::Less => println!("Too small!"),
			Ordering::Greater => println!("Too big!"),
			Ordering::Equal => {
				println!("You win!");
				break;
			}
		}
	}
    
}
