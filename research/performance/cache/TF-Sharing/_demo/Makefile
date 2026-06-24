
A?=.
B?=a.out
ARGS?=

all: dynamic

# make
# b_makedot.py
static:
	cd $(A) && dbg-viewtype $(B) type.txt vars.txt
	cd $(A) && sudo /home/neu/bin/AMD64/Lin64/perfparse $(B)
	python ./b_makedot.py $(A)/hitm.txt $(A)/dfg_global.dot $(A)/all_global.dot

# sudo make
dynamic:
	cd $(A) && /home/neu/bin/AMD64/Lin64/dbg-viewtype $(B) type.txt vars.txt
	@cd $(A) && ( \
		WORK_DIR=$$(cd "$(A)" && pwd); \
		TARGET_BIN="$$WORK_DIR/$(B)"; \
		MEMSCOPE_BIN="/home/neu/exp/memscope-dev/build/src/collector/memscope-collect"; \
		MEMSCOPE_BPF="/home/neu/exp/memscope-dev/build/src/bpf/memscope.bpf.o"; \
		PERFPARSE="/home/neu/bin/AMD64/Lin64/perfparse"; \
		\
		echo "  启动目标程序并暂停..."; \
		$$TARGET_BIN $(ARGS) & \
		TARGET_PID=$$!; \
		kill -STOP $$TARGET_PID; \
		echo "  目标程序 PID: $$TARGET_PID"; \
		\
		echo "  启动 memscope..."; \
		sudo -S $$MEMSCOPE_BIN -p $$TARGET_PID -B $$MEMSCOPE_BPF -d 60 -o $$WORK_DIR/memscope_allocs.csv & \
		MEMSCOPE_PID=$$!; \
		echo "  memscope PID: $$MEMSCOPE_PID"; \
		\
		echo "  启动 perf c2c..."; \
		sudo -S perf c2c record -p $$TARGET_PID -o $$WORK_DIR/perf.data & \
		PERFC2C_PID=$$!; \
		echo "  perf c2c PID: $$PERFC2C_PID"; \
		\
		sleep 1; \
		\
		echo "  ✓ 采集器已就绪，恢复目标程序..."; \
		kill -CONT $$TARGET_PID; \
		\
		echo "  等待目标程序完成..."; \
		wait $$TARGET_PID 2>/dev/null || true; \
		echo "  目标程序已完成"; \
		\
		sleep 2; \
		echo "  停止采集器..."; \
# 		sudo -S kill $$PERFC2C_PID 2>/dev/null || true; \
# 		sudo -S kill $$MEMSCOPE_PID 2>/dev/null || true; \
		\
		wait $$PERFC2C_PID 2>/dev/null || true; \
		wait $$MEMSCOPE_PID 2>/dev/null || true; \
		\
		echo "  生成 perf.txt..."; \
		perf script -i $$WORK_DIR/perf.data > $$WORK_DIR/perf.txt; \
		echo "  运行 perfparse 分析..."; \
		sudo -S $$PERFPARSE $$TARGET_BIN --no-record; \
		echo "  ✅ 动态分析完成！" \
	) 2>&1
	python ./b_makedot.py $(A)/hitm.txt $(A)/dfg_global.dot $(A)/all_global.dot

	# make heap:
	@cd $(A) && ( \
		WORK_DIR=$$(cd "$(A)" && pwd); \
		TARGET_BIN="$$WORK_DIR/$(B)"; \
		PERFPARSE_A="/home/neu/bin/AMD64/Lin64/perfparse_a"; \
		\
		echo "  运行 perfparse_a 处理 hitm.txt..."; \
		sudo -S $$PERFPARSE_A $$TARGET_BIN; \
		echo "  ✅ heap 分析完成！生成了 hitm_a.txt 和 hitm_aa.txt" \
	) 2>&1

	# make heap dfg
	@cd $(A) && ( \
		WORK_DIR=$$(cd "$(A)" && pwd); \
		TARGET_BIN="$$WORK_DIR/$(B)"; \
		MEMSCOPE_RESOLVE="/home/neu/exp/memscope-dev/build/src/resolver/memscope-resolve"; \
		\
		echo "  运行 memscope-resolve 生成 memscope_resolved.csv..."; \
		$$MEMSCOPE_RESOLVE batch -b "$$TARGET_BIN" -f "$$WORK_DIR/memscope_allocs.csv" -o "$$WORK_DIR/memscope_resolved.csv" || \
			sudo -S $$MEMSCOPE_RESOLVE batch -b "$$TARGET_BIN" -f "$$WORK_DIR/memscope_allocs.csv" -o "$$WORK_DIR/memscope_resolved.csv"; \
		echo "  ✅ memscope-resolve 完成！" \
	) 2>&1
	python /home/neu/exp/cacheline_fs_analyzer.py --hitm $(A)/hitm_a.txt --memscope $(A)/memscope_resolved.csv --binary $(A)/$(B) --work-dir $(A) --output-dir $(A)
	python /home/neu/exp/overlay_fs_to_dot2.py $(A)

# A and B
dynamic_ab:
	cd $(A) && /home/neu/bin/AMD64/Lin64/dbg-viewtype $(B) type.txt vars.txt
	@cd $(A) && ( \
		WORK_DIR=$$(cd "$(A)" && pwd); \
		TARGET_BIN="$$WORK_DIR/$(B)"; \
		FSPARSE_A="/home/neu/exp/alicewiki/research/performance/cache/TF-Sharing/fsparse_a/target/release/fsparse_a"; \
		PERFPARSE="/home/neu/bin/AMD64/Lin64/perfparse"; \
		\
		echo "  启动目标程序并暂停..."; \
		$$TARGET_BIN $(ARGS) & \
		TARGET_PID=$$!; \
		kill -STOP $$TARGET_PID; \
		echo "  目标程序 PID: $$TARGET_PID"; \
		\
		echo "  启动 fsparse_a..."; \
		sudo -S $$FSPARSE_A --binary $$TARGET_BIN --pid $$TARGET_PID --duration 60 --output $$WORK_DIR/memscope_resolved.csv trace & \
		FSPARSE_PID=$$!; \
		echo "  fsparse_a PID: $$FSPARSE_PID"; \
		\
		echo "  启动 perf c2c..."; \
		sudo -S perf c2c record -p $$TARGET_PID -o $$WORK_DIR/perf.data & \
		PERFC2C_PID=$$!; \
		echo "  perf c2c PID: $$PERFC2C_PID"; \
		\
		sleep 1; \
		\
		echo "  ✓ 采集器已就绪，恢复目标程序..."; \
		kill -CONT $$TARGET_PID; \
		\
		echo "  等待目标程序完成..."; \
		wait $$TARGET_PID 2>/dev/null || true; \
		echo "  目标程序已完成"; \
		\
		sleep 2; \
		echo "  停止采集器..."; \
		\
		wait $$PERFC2C_PID 2>/dev/null || true; \
		wait $$FSPARSE_PID 2>/dev/null || true; \
		\
		echo "  生成 perf.txt..."; \
		perf script -i $$WORK_DIR/perf.data > $$WORK_DIR/perf.txt; \
		echo "  运行 perfparse 分析..."; \
		sudo -S $$PERFPARSE $$TARGET_BIN --no-record; \
		echo "  ✅ 动态分析完成！" \
	) 2>&1
	python ./b_makedot.py $(A)/hitm.txt $(A)/dfg_global.dot $(A)/all_global.dot

	# make heap:
	@cd $(A) && ( \
		WORK_DIR=$$(cd "$(A)" && pwd); \
		TARGET_BIN="$$WORK_DIR/$(B)"; \
		PERFPARSE_A="/home/neu/bin/AMD64/Lin64/perfparse_a"; \
		\
		echo "  运行 perfparse_a 处理 hitm.txt..."; \
		sudo -S $$PERFPARSE_A $$TARGET_BIN; \
		echo "  ✅ heap 分析完成！生成了 hitm_a.txt 和 hitm_aa.txt" \
	) 2>&1

	python /home/neu/exp/cacheline_fs_analyzer.py --hitm $(A)/hitm_a.txt --memscope $(A)/memscope_resolved.csv --binary $(A)/$(B) --work-dir $(A) --output-dir $(A)
	python /home/neu/exp/overlay_fs_to_dot2.py $(A)

clean:
	rm -f $(A)/hitm*.txt $(A)/perf.data $(A)/perf.txt $(A)/memscope_allocs*.csv $(A)/memscope_resolved.csv
