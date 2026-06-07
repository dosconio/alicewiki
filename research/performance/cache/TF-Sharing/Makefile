ubinpath ?= /tmp

all: install

install: perfparse perfparse_a
	sudo install -D perfparse/target/debug/perfparse $(ubinpath)/AMD64/Lin64/perfparse
	sudo install -D perfparse_a/target/debug/perfparse_a $(ubinpath)/AMD64/Lin64/perfparse_a

perfparse:
	cd perfparse && cargo build

perfparse_a:
	cd perfparse_a && cargo build

.PHONY: clean perfparse perfparse_a
clean:
	cd perfparse && cargo clean
	cd perfparse_a && cargo clean
	rm -f $(ubinpath)/AMD64/Lin64/perfparse
	rm -f $(ubinpath)/AMD64/Lin64/perfparse_a
