# SD card reads corrupt under ESP-IDF v6.0.2 on the Tanmatsu

Investigation notes, 2026-09-06. Written after migrating `tanmatsu-graceloader`
from ESP-IDF v5.5.1 to v6.0.2.

**Status: root cause attributed but not proven.** A workaround is in place and
committed (`ee85b37`). The attribution is [espressif/esp-idf#16233][16233],
which Espressif filed and then closed *"Resolution: NA"* — i.e. acknowledged
and not fixed.

[16233]: https://github.com/espressif/esp-idf/issues/16233

---

## 1. Symptom

After the v6.0.2 migration, grace apps would not load from the SD card.
`kbelf` reported a malformed ELF (`dynsym=0`, garbled symbol names) and
graceloader bounced back to the launcher.

The same app, installed to `/int` (FAT-on-flash via wear levelling), loaded and
ran perfectly. Same binary, same loader, same everything except the storage
backend. That was the first clue that this was a storage-layer problem and not
an ELF/relocation problem.

Everything else about the SD card worked: mounting, directory listing, reading
`metadata.json`, reading icons, reading and writing save files. Only the app
`.so` load failed.

## 2. Environment

| | |
|---|---|
| SoC | ESP32-P4 (rev v1.3) |
| ESP-IDF (broken) | v6.0.2, commit `7101770dc6db2667b3c477cc31365dd1acd6db4e` |
| ESP-IDF (working) | v5.5.1, commit `fcae32885b0296b32044cb99ecbdc50d98dddb83` |
| Toolchain | gcc 15.2 (6.0.2) vs gcc 14.2 (5.5.1) |
| PSRAM | 32 MB, `CONFIG_SPIRAM_SPEED_200M=y` |
| L1 data cache line | 64 B (`CONFIG_CACHE_L1_CACHE_LINE_SIZE`, fixed on P4) |
| L2 cache | 128 KB, 64 B line (`CONFIG_CACHE_L2_CACHE_SIZE=0x20000`) |
| Assertions | **enabled** (`CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y`, level 2) |
| C2M chunked msync | **off** (`CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS` not set) |
| Interrupt WDT | 300 ms |
| SD card | SDMMC **slot 0**, 4-bit, 40 MHz (`SDMMC_FREQ_HIGHSPEED`) |
| Radio | ESP32-C6 over SDIO on SDMMC **slot 1**, via ESP-Hosted |

The last two rows matter more than anything else in this document.

## 3. The measurement that pinned down the failure

The failure was characterised by instrumenting `kbelfx_load()` with three
FNV-1a checksums over the same byte range:

1. `file` — the segment re-read in small chunks through an internal-RAM buffer
2. `raw` — the destination in PSRAM, read normally (through the cache)
3. `afterInval` — the destination re-read after an explicit
   `cache_hal_invalidate_addr()`

Results:

- The file on the card was byte-perfect. Its `md5` matched the build artifact,
  and `file` was **stable at `0xf40e38fd` across every run**.
- `mem` never matched `file`, and **varied from run to run**.
- Initially `raw != afterInval != file` — three different values. That means
  dirty cache lines (left by `aligned_alloc` / the BSS `memset`) were being
  evicted *over* data the DMA had already written.
- After adding an explicit writeback+invalidate **before** the read,
  `raw == afterInval != file` — stable, reproducible, and still wrong.

That last state is the important one. With the cache demonstrably coherent in
both directions, the bytes sitting in PSRAM still did not match the bytes on
the card. **The DMA delivered wrong data.** This is not a cache-coherency bug.

### Why only the ELF load

The ELF segment is roughly 150 KB and is the only read in the entire firmware
that is (a) large, (b) sector-aligned, and (c) destined for a big PSRAM buffer.
Everything else — metadata, icons, save files — reads a few hundred bytes to a
few KB into internal RAM. FatFs hands a large sector-aligned read straight to
`sdmmc_read_sectors()` with the caller's buffer as the DMA destination, so the
ELF load was the only thing exercising the failing path.

## 4. The workaround (committed, `ee85b37`)

`components/kbelf/src/kbelfx.c` — read through a 4 KB internal-RAM staging
buffer and copy into PSRAM with the CPU:

```c
long nread = 0;
uint8_t* stage = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
if (stage) {
    while (nread < (long)file_size) {
        size_t want = (size_t)file_size - (size_t)nread;
        if (want > 4096) want = 4096;
        size_t got = fread(stage, 1, want, fd);
        if (got == 0) break;
        memcpy((void*)(laddr + nread), stage, got);
        nread += (long)got;
        if (got < want) break;
    }
    free(stage);
} else {
    nread = fread((void*)laddr, 1, file_size, fd);
}
```

This works, confirmed on hardware. It changes two things at once — DMA now
lands in internal RAM instead of PSRAM, *and* every transfer is ≤ 4 KB — so it
does not by itself discriminate between the competing explanations.

Also committed alongside it:

- `components/kbelf/Kconfig` — ports the launcher's "Fast FATFS I/O" options
  (`FATFS_USE_FASTOPEN`, `FATFS_MAX_FILES_OPEN`, `FATFS_STDIO_BUF_SIZE`).
- `sdkconfigs/tanmatsu` — `CONFIG_FATFS_USE_FASTOPEN=y`,
  `CONFIG_FATFS_MAX_FILES_OPEN=8`, `CONFIG_FATFS_STDIO_BUF_SIZE=8192`.
  This routes *buffered* stdio reads through an 8 KB internal DMA-capable
  buffer, which incidentally protects every read under 8 KB.
- `lib/kbelf` submodule bump to `9be1443` — cache sync moved to *before*
  relocation (relocation parses the freshly-loaded segments, so the loader must
  see real memory at that point), plus cherry-pick `8af8bfd` from upstream
  (`DTB_LOCAL`, builtin libs evaluated last).

## 5. Root cause attribution

### ESP-IDF issue #16233 — SD card and ESP-Hosted cannot share the SDMMC controller

ESP-IDF v6.0 rewrote the SDMMC host driver: commit `402bf0c`
*"feat(sd): sd host driver layer driver NG"*, which replaced
`esp_driver_sdmmc/src/sdmmc_host.c`'s single global host with a
controller/slot model in `sd_host_sdmmc.c` + `sd_trans_sdmmc.c`.

That rewrite hardcoded `SDMMC_LL_HOST_CTLR_NUMS` to `1U`. Interrupts are
allocated from `ETS_SDIO_HOST_INTR_SOURCE` with no way to distinguish
controllers (`sd_host_sdmmc.c:102`), and `sd_host_claim_controller()` permits
exactly one controller instance for the whole chip.

On the Tanmatsu both SD devices are on that one controller:

- SD card → slot 0 (`main/sdcard.c:76`, native IOMUX pins GPIO39–44)
- ESP32-C6 radio → slot 1 (`CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_*_SLOT_1`)

ESP-Hosted initialises **before** `app_main()` and claims the controller. The
boot log shows it:

```
I (1411) H_SDIO_DRV: sdio_data_to_rx_buf_task started
I (1411) main_task: Started on CPU0
I (1411) main_task: Calling app_main()
```

Note this happens **whether or not the application ever starts Wi-Fi**.
Graceloader contains no `esp_wifi_*` calls at all, and the SDIO transport still
comes up, because `CONFIG_ESP_HOSTED_ENABLED=y` and
`CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y`.

Consequently our `esp_vfs_fat_sdmmc_mount()` gets
`ESP_ERR_NOT_FOUND` — *"no available sd host controller"* — from
`sd_host_create_sdmmc_controller()`. The workaround, in `main/sdcard.c` and
identical to Espressif's own `host_sdcard_with_hosted` example, is to hand the
mount no-op init/deinit callbacks and let ESP-Hosted own the controller:

```c
#if defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
static esp_err_t sdmmc_host_init_noop(void)   { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_noop(void) { return ESP_OK; }
#endif
...
host.init   = &sdmmc_host_init_noop;
host.deinit = &sdmmc_host_deinit_noop;
```

**That workaround gets the card mounted. It does nothing about the sharing.**
After it, the SD card and the radio share:

- one `sdmmc_desc_t` DMA descriptor ring (`ctlr->dma_desc`, 4 descriptors)
- one `cur_transfer` state (`ptr`, `size_remaining`, `next_desc`)
- one ISR (`sd_host_isr`), which derives its slot from `ctlr->cur_slot_id`
- one controller mutex

and v6.0.2 additionally reprograms clock, bus width, sampling mode, delay phase
and delay line at the **start of every transaction** (`sd_trans_sdmmc.c:500-509`)
— all of which v5.5.1 did once at init, because it never had to switch between
slots.

### Why the size threshold fits

The descriptor ring is 4 entries (`SD_HOST_SDMMC_DMA_DESC_CNT`, `sd_host_private.h:68`)
and `SDMMC_DMA_MAX_BUF_LEN` is 4096. So a transfer up to **16 KB** is fully
described by the initial `sd_host_dma_prepare()` fill and never needs a
mid-transfer, ISR-driven descriptor refill. Above 16 KB it does — and that is
where the shared ring, the shared `cur_transfer` and the shared ISR are being
mutated while a second device is live on the same controller.

Every read that worked is under that threshold. The one that failed is 150 KB.

**This attribution is not proven.** We did not isolate the exact instruction
sequence that corrupts the data. What we have is: a documented, unfixed
upstream defect that creates exactly this sharing; a size threshold that
matches the driver's descriptor geometry; and an exhaustive elimination of
every other difference between the two IDF versions (§6).

## 6. Ruled out

Every one of these was diffed between the two IDF trees and found functionally
identical, or tested on hardware and disproven.

| Candidate | Verdict |
|---|---|
| FatFs `ff.c` read path | Identical. Only diffs are `FF_USE_DYN_BUFFER` allocation lifetime. |
| `diskio_sdmmc.c` | Identical read path; 6.0.2 only adds `ff_diskio_get_pdrv_cnt_card()`. |
| `sdmmc_read_sectors()` dispatch | Identical aligned/unaligned branch, identical `SOC_SDMMC_PSRAM_DMA_CAPABLE` guard. |
| `check_buffer_alignment()` | Identical logic (`sdmmc_host.c:1287` vs `sd_host_sdmmc.c:455`). |
| Data-buffer cache sync | Identical: C2M over `buflen` before, M2C over `buflen` after. |
| `esp_cache_msync()` | Body unchanged. Only diff is `ESP_ERR_NOT_SUPPORTED` for non-cacheable ranges, plus removal of the deprecated `esp_cache_aligned_*` helpers. |
| `cache_hal_invalidate_addr` / `writeback_addr` | Identical. |
| P4 `cache_ll.h` | Identical apart from a parameter rename. |
| SDMMC interrupt masks (`DCRC`, `SBE`, `EBE`, `DTO`) | Identical. |
| `process_events()` / error handling | Identical apart from the type/naming refactor. |
| **DMA descriptor alignment** | Not a problem. `sizeof(sdmmc_desc_t) == 64` and `MALLOC_CAP_DMA` forces cache-line alignment in IDF 6 (`heap_align_hw.c:25`, `CAPS_NEEDING_ALIGNMENT`). With assertions enabled, the descriptor `esp_cache_msync()` calls demonstrably succeed. |
| **`host.dma_aligned_buffer`** | **Tested on hardware, disproven.** See §7. |
| PSRAM / cache sdkconfig | Unchanged across the migration. |

### The theory that looked best and was wrong

`sd_protocol_types.h` in **5.5.1**:

```c
void* dma_aligned_buffer; /*!< Leave it NULL. Reserved for cache aligned buffers for SDIO mode */
```

In **6.0.2** the same field is repurposed:

> **Cache aligned buffer for multi-block RW and IO commands.** Temporary buffer
> for multi-block read/write transactions to/from unaligned buffers. The number
> of blocks transferred per chunk is controlled by
> `unaligned_multi_block_rw_max_chunk_size`.

`sdmmc_cmd.c` references it **0 times in 5.5.1 and 10 times in 6.0.2**.

`main/sdcard.c` sets it to a 2 KB buffer. That line was inert on 5.5.1 and went
live on 6.0.2, with `blocks_per_read` derived from
`heap_caps_get_allocated_size()` of *our* allocation rather than a buffer the
driver sized itself. Espressif's own reference for this exact configuration
leaves the field NULL.

Textbook silent semantic change at exactly the version boundary where the bug
appeared. **It is not the cause** — see §7.

## 7. Hardware test performed

Branch `test/no-dma-aligned-buffer` (since deleted), two changes at once:

1. `main/sdcard.c` — `host.dma_aligned_buffer` left NULL, matching Espressif's
   reference implementation.
2. `components/kbelf/src/kbelfx.c` — `kbelfx_load()` reverted to a plain
   `fread()` straight into PSRAM, as it was on 5.5.1.

**Result: still fails.** The app does not load. `dma_aligned_buffer` is
exonerated; the corruption is upstream of our host configuration.

The device was restored to `ee85b37` and reinstalled.

## 8. A separate, genuine regression found along the way

Unrelated to the corruption, but real and (as far as we can tell) unreported.

`sd_host_fill_dma_descriptors()` wraps the descriptor ring on its `num_desc`
**argument**:

```c
/* esp_driver_sdmmc/src/sd_trans_sdmmc.c:125,131 */
desc->next_desc_ptr  = last ? NULL : slot->ctlr->dma_desc + ((next + 1) % num_desc);
cur_trans->next_desc = (cur_trans->next_desc + 1) % num_desc;
```

v5.5.1 used the constant ring size in both places:

```c
/* esp_driver_sdmmc/src/sdmmc_host.c:1091,1097 */
desc->next_desc_ptr    = last ? NULL : &s_dma_desc[(next + 1) % SDMMC_DMA_DESC_CNT];
s_host_ctx.next_desc   = (s_host_ctx.next_desc + 1) % SDMMC_DMA_DESC_CNT;
```

The ISR refill calls it with the count of *free* descriptors
(`sd_host_sdmmc.c:806-808`), so mid-transfer the ring wraps at 1, 2 or 3
instead of 4. The chain loops back on itself early and the driver's bookkeeping
pointer stalls.

Simulated both implementations across randomised ISR-latency patterns:

- Refilling after *every* descriptor: benign (degenerates to a one-descriptor
  chain; the data pointer is monotonic so bytes still land correctly).
- Realistic latency, **without** modelling resume-on-suspend: 190/500 runs
  corrupt, up to 134 KB wrong. v5.5.1: 0/500.
- Realistic latency, **with** resume-on-suspend modelled (which is what the
  driver actually does on DU/NI): 0/500 corrupt for both.

So it costs round-trips, not data. Additionally, this build has assertions
enabled, and `sd_host_fill_dma_descriptors()` contains
`assert(!desc->owned_by_idmac)` — the firmware would have aborted if a live
descriptor were ever clobbered. Worth reporting upstream on its own merits.

## 9. Practical guidance

- **Any SD read larger than ~16 KB straight into a PSRAM destination is
  suspect** on this platform + IDF combination. Chunk it, or stage through
  internal RAM.
- Reads under 8 KB go through the stdio buffer (`CONFIG_FATFS_STDIO_BUF_SIZE=8192`,
  internal DMA-capable) and are safe automatically.
- Candidates worth auditing for the same pattern: `tanmatsu-videoplayer`,
  `tanmatsu-camera`, and any grace app streaming large assets off the card.
- The workaround in `kbelfx_load()` should stay. It is not papering over a bug
  in our code — it is working around an upstream defect that Espressif has
  declined to fix.

## 10. What would actually settle it

None of these were run; recording them so the next person does not start over.

1. **Build with ESP-Hosted compiled out entirely** (not just Wi-Fi unused —
   `CONFIG_ESP_HOSTED_ENABLED=n`), so nothing else claims the controller, then
   do a large direct-to-PSRAM read. If it comes back clean, #16233 is confirmed
   and the case is closed.
2. Instrument `sd_host_isr()` to count DU/NI events and log
   `ctlr->cur_slot_id` transitions during a large slot-0 read, to see whether
   slot-1 activity actually interleaves.
3. Patch `sd_trans_sdmmc.c:125,131` to use `slot->ctlr->dma_desc_num` and
   re-test — cheap, and fixes a real bug regardless of the outcome.

> Note: on-device instrumentation was attempted once during this investigation
> and was a mistake. A diagnostic added to `app_main()` put the board into a
> watchdog reboot loop (`rst:0x7 HP_SYS_HP_WDT_RESET`) and produced no usable
> output, because graceloader switches USB out of badgelink mode at the top of
> `app_main()` and the log is lost. If instrumenting again: write results to
> `/int`, not the serial console, and bound the runtime against the 300 ms
> interrupt watchdog (note `CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS` is off,
> so a large `esp_cache_msync()` runs with interrupts disabled).

## 11. References

- [espressif/esp-idf#16233 — SDMMC and ESP-Hosted SDIO cannot coexist][16233]
- [`sd_trans_sdmmc.c` commit history](https://github.com/espressif/esp-idf/commits/master/components/esp_driver_sdmmc/src/sd_trans_sdmmc.c) — `402bf0c` is the rewrite
- `managed_components/nicolaielectronics__esp-hosted-tanmatsu/examples/host_sdcard_with_hosted/`
  — Espressif's reference workaround, and the README that points at #16233
- [ESP-IDF 6.0 peripheral migration guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/peripherals.html)
- [Memory synchronization on ESP32-P4](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/mm_sync.html)
