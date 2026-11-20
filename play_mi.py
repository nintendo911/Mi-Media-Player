"""
=============================================================================
 Okami NT Media Player & Audio Tool (Windows Host Runner)
 Copyright (C) 2026 NETJAN Inc. All rights reserved.
=============================================================================
 Giúp chạy và kiểm thử MiMediaPlayer.okami trên hệ điều hành Windows:
   1. Phát mọi định dạng âm thanh (.mi, .wav, .mp3, .flac, .ogg, .raw,...)
      trực tiếp ra loa máy tính Windows (in-memory, 0 tạo file tạm).
   2. Hỗ trợ đa kênh: Tự động Upmix Mono -> Stereo, Downmix 5.1 Surround -> Stereo
      theo chuẩn quốc tế ITU-R BS.775, chống vỡ tiếng.
   3. Hỗ trợ đa luồng: Phát âm thanh nền không nghẽn bằng luồng riêng (threading),
      hiển thị thanh tiến trình live trực quan.
   4. Đóng gói chuẩn nén .mi (MI01 Lookahead ADPCM 4:1) cho Okami NT.
   5. Tương thích trực tiếp với lệnh MiMediaPlayer.okami trên Windows.
=============================================================================
"""

import sys
import os
import time
import struct
import math
import json
import shutil
import subprocess
import threading
import wave
import winsound

# ===========================================================================
# 1. BẢNG THAM SỐ CHUẨN NÉN OKAMI ADPCM 4:1 (2 LÀN SONG SONG)
# ===========================================================================
STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
    8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767
]

INDEX_ADJUST = [-1, -1, -1, -1, 2, 4, 6, 8]

# Khởi tạo bảng tra sẵn tốc độ cao (Precomputed LUT)
DELTA_TABLE = [0] * (89 * 16)
NEXT_STEP_TABLE = [0] * (89 * 16)

for step_idx in range(89):
    step_val = STEP_TABLE[step_idx]
    for nibble in range(16):
        diff = step_val >> 3
        if nibble & 4: diff += step_val
        if nibble & 2: diff += step_val >> 1
        if nibble & 1: diff += step_val >> 2
        if nibble & 8: diff = -diff
        DELTA_TABLE[step_idx * 16 + nibble] = diff

        next_idx = step_idx + INDEX_ADJUST[nibble & 7]
        if next_idx < 0: next_idx = 0
        elif next_idx > 88: next_idx = 88
        NEXT_STEP_TABLE[step_idx * 16 + nibble] = next_idx


def clamp16(val):
    if val > 32767: return 32767
    if val < -32768: return -32768
    return int(val)


def to_wsl_path(path):
    """Chuyển đổi đường dẫn Windows (D:\\...) sang đường dẫn WSL (/mnt/d/...) chuẩn xác"""
    abs_p = os.path.abspath(path)
    drive, tail = os.path.splitdrive(abs_p)
    if drive:
        d = drive[0].lower()
        t = tail.replace('\\', '/')
        return f"/mnt/{d}{t}"
    return abs_p.replace('\\', '/')


# ===========================================================================
# 2. XỬ LÝ MA TRẬN ĐA KÊNH (MULTI-CHANNEL MATRIX DOWNMIX & UPMIX)
# ===========================================================================
def mix_to_stereo(pcm_bytes, channels):
    """
    Chuyển đổi âm thanh bất kỳ số kênh (1..6) sang chuẩn Stereo 2 kênh 16-bit:
    - 1 kênh (Mono): Upmix nhân đôi ra L/R.
    - 2 kênh (Stereo): Giữ nguyên.
    - 4 kênh (Quad) & 5.1 Surround (6 kênh): Hòa âm theo chuẩn ITU-R BS.775.
    """
    if channels == 2:
        return pcm_bytes

    total_samples = len(pcm_bytes) // 2
    frames = total_samples // channels
    samples = struct.unpack(f'<{total_samples}h', pcm_bytes[:frames * channels * 2])
    out_samples = [0] * (frames * 2)

    if channels == 1:
        # Upmix Mono -> Stereo
        for i in range(frames):
            s = samples[i]
            out_samples[i * 2]     = s
            out_samples[i * 2 + 1] = s
    elif channels == 4:
        # Quad (FL, FR, BL, BR) -> Stereo
        for i in range(frames):
            fl, fr, bl, br = samples[i * 4 : i * 4 + 4]
            l = clamp16(fl + int(bl * 0.7071))
            r = clamp16(fr + int(br * 0.7071))
            out_samples[i * 2]     = l
            out_samples[i * 2 + 1] = r
    elif channels >= 6:
        # 5.1 Surround (FL, FR, FC, LFE, BL, BR) -> Stereo ITU-R BS.775
        for i in range(frames):
            base = i * channels
            fl  = samples[base]
            fr  = samples[base + 1]
            fc  = samples[base + 2]
            lfe = samples[base + 3]
            bl  = samples[base + 4]
            br  = samples[base + 5]

            c_part   = int(fc * 0.7071)
            lfe_part = int(lfe * 0.5)
            l = clamp16(fl + c_part + int(bl * 0.7071) + lfe_part)
            r = clamp16(fr + c_part + int(br * 0.7071) + lfe_part)
            out_samples[i * 2]     = l
            out_samples[i * 2 + 1] = r
    else:
        # Kênh khác: lấy 2 kênh đầu
        for i in range(frames):
            out_samples[i * 2]     = samples[i * channels]
            out_samples[i * 2 + 1] = samples[i * channels + 1]

    return struct.pack(f'<{len(out_samples)}h', *out_samples)


# ===========================================================================
# 3. THUẬT TOÁN ĐÓNG GÓI .MI (LOOKAHEAD TRELLIS 4:1)
# ===========================================================================
def encode_adpcm_sample_lookahead(s0, s1, pred, idx):
    best_code = 0
    best_err = float('inf')
    step = STEP_TABLE[idx]

    diff0 = s0 - pred
    sign = 8 if diff0 < 0 else 0
    mag0 = abs(diff0)

    code0_base = sign
    if mag0 >= step:
        code0_base |= 4
        mag0 -= step
    if mag0 >= (step >> 1):
        code0_base |= 2
        mag0 -= step >> 1
    if mag0 >= (step >> 2):
        code0_base |= 1

    candidate_codes = [code0_base]
    if (code0_base & 7) < 7: candidate_codes.append(code0_base + 1)
    if (code0_base & 7) > 0: candidate_codes.append(code0_base - 1)

    for c0 in candidate_codes:
        next_p0 = clamp16(pred + DELTA_TABLE[idx * 16 + c0])
        next_idx0 = NEXT_STEP_TABLE[idx * 16 + c0]
        err0 = (s0 - next_p0) ** 2

        step1 = STEP_TABLE[next_idx0]
        diff1 = s1 - next_p0
        code1 = 8 if diff1 < 0 else 0
        mag1 = abs(diff1)
        if mag1 >= step1:
            code1 |= 4
            mag1 -= step1
        if mag1 >= (step1 >> 1):
            code1 |= 2
            mag1 -= step1 >> 1
        if mag1 >= (step1 >> 2):
            code1 |= 1

        next_p1 = clamp16(next_p0 + DELTA_TABLE[next_idx0 * 16 + code1])
        err1 = (s1 - next_p1) ** 2

        total_err = err0 + err1
        if total_err < best_err:
            best_err = total_err
            best_code = c0

    next_pred = clamp16(pred + DELTA_TABLE[idx * 16 + best_code])
    next_idx = NEXT_STEP_TABLE[idx * 16 + best_code]
    return best_code, next_pred, next_idx


def encode_pcm_to_mi(pcm_bytes, sample_rate, channels, out_mi_path):
    bytes_per_frame = channels * 2
    total_frames = len(pcm_bytes) // bytes_per_frame
    if total_frames == 0:
        raise ValueError("Dữ liệu PCM rỗng!")

    samples = struct.unpack(f'<{total_frames * channels}h', pcm_bytes[:total_frames * bytes_per_frame])

    def get_s(f, c):
        if f >= total_frames: f = total_frames - 1
        return samples[f * channels + c]

    N = 512       # lane_nibbles
    L = N + 1     # lane_frames = 513
    F = 2 * L     # block_frames = 1026
    blocks = (total_frames + F - 1) // F

    idx = [0] * channels
    payload = bytearray()

    for blk in range(blocks):
        base = blk * F
        for c in range(channels):
            pred_a = get_s(base, c)
            hdr_a = struct.pack('<hh', pred_a, idx[c])
            nib_a = []
            for i in range(1, N + 1):
                s0 = get_s(base + i, c)
                s1 = get_s(base + i + 1, c)
                code, pred_a, idx[c] = encode_adpcm_sample_lookahead(s0, s1, pred_a, idx[c])
                nib_a.append(code)

            pred_b = get_s(base + L, c)
            hdr_b = struct.pack('<hh', pred_b, idx[c])
            nib_b = []
            for i in range(1, N + 1):
                s0 = get_s(base + L + i, c)
                s1 = get_s(base + L + i + 1, c)
                code, pred_b, idx[c] = encode_adpcm_sample_lookahead(s0, s1, pred_b, idx[c])
                nib_b.append(code)

            payload.extend(hdr_a)
            payload.extend(hdr_b)
            for i in range(N):
                byte_val = (nib_a[i] & 0x0F) | ((nib_b[i] & 0x0F) << 4)
                payload.append(byte_val)

    hdr = struct.pack(
        '<4sHH I HH I I I I',
        b'MI01',
        channels,
        16,
        sample_rate,
        1,          # MI_CODEC_ADPCM4
        F,          # block_frames = 1026
        total_frames,
        len(pcm_bytes),
        len(payload),
        0           # channel_mask
    )

    with open(out_mi_path, "wb") as f:
        f.write(hdr)
        f.write(payload)

    return total_frames, len(hdr) + len(payload)


# ===========================================================================
# 4. TỰ ĐỘNG NHẬN DIỆN & NẠP MỌI ĐỊNH DẠNG ÂM THANH
# ===========================================================================
def auto_detect_raw_channels(pcm_bytes):
    num_samples = len(pcm_bytes) // 2
    if num_samples < 200: return 2
    test_len = min(num_samples, 8000)
    samples = struct.unpack(f'<{test_len}h', pcm_bytes[:test_len * 2])
    diff1 = sum(abs(samples[i] - samples[i - 1]) for i in range(1, test_len))
    diff2 = sum(abs(samples[i] - samples[i - 2]) for i in range(2, test_len))
    if diff2 == 0: return 1
    return 1 if (diff1 / diff2) < 0.72 else 2


def load_any_audio(file_path):
    file_path = os.path.normpath(os.path.abspath(file_path))
    if not os.path.exists(file_path):
        raise FileNotFoundError(f"Không tìm thấy file: '{file_path}'")

    ext = os.path.splitext(file_path)[1].lower()

    # 1. PCM WAV files that the standard library can read directly.
    if ext == ".wav":
        try:
            with wave.open(file_path, "rb") as wf:
                ch = wf.getnchannels()
                rate = wf.getframerate()
                width = wf.getsampwidth()
                raw = wf.readframes(wf.getnframes())
                if width == 2:
                    return raw, rate, ch
                elif width == 1:
                    s8 = struct.unpack(f'{len(raw)}B', raw)
                    s16 = [(x - 128) << 8 for x in s8]
                    return struct.pack(f'<{len(s16)}h', *s16), rate, ch
                elif width == 3:
                    total_s = len(raw) // 3
                    s16 = [0] * total_s
                    for i in range(total_s):
                        b1 = raw[i * 3 + 1]
                        b2 = raw[i * 3 + 2]
                        val = (b2 << 8) | b1
                        if b2 & 0x80: val -= 65536
                        s16[i] = val
                    return struct.pack(f'<{total_s}h', *s16), rate, ch
                elif width == 4:
                    total_s = len(raw) // 4
                    samples = struct.unpack(f'<{total_s}i', raw)
                    s16 = [clamp16(sample >> 16) for sample in samples]
                    return struct.pack(f'<{total_s}h', *s16), rate, ch
        except (wave.Error, EOFError, OSError, struct.error):
            pass

    # 2. Headerless PCM has no metadata, so keep the existing 44.1 kHz assumption.
    if ext in (".raw", ".pcm"):
        with open(file_path, "rb") as f: pcm = f.read()
        if not pcm or len(pcm) % 2:
            raise ValueError("File RAW/PCM phải có dữ liệu PCM 16-bit hợp lệ.")
        return pcm, 44100, auto_detect_raw_channels(pcm)

    # 3. Decode formats supported by FFmpeg to a predictable playback/encode format.
    ffmpeg = shutil.which("ffmpeg")
    command_prefix = [ffmpeg] if ffmpeg else None
    input_path = file_path
    if not command_prefix and shutil.which("wsl"):
        try:
            probe = subprocess.run(
                ["wsl", "ffmpeg", "-version"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            if probe.returncode == 0:
                command_prefix = ["wsl", "ffmpeg"]
                input_path = to_wsl_path(file_path)
        except OSError:
            pass

    if not command_prefix:
        raise RuntimeError(
            "Cần cài FFmpeg và thêm ffmpeg vào PATH để mở định dạng này. "
            "RAW/PCM và WAV PCM vẫn được hỗ trợ trực tiếp."
        )

    sample_rate, channels = 44100, 2
    ffprobe = shutil.which("ffprobe")
    probe_prefix = [ffprobe] if ffprobe else (["wsl", "ffprobe"] if not ffmpeg else None)
    if probe_prefix:
        try:
            probe = subprocess.run(
                probe_prefix + [
                    "-v", "error", "-select_streams", "a:0",
                    "-show_entries", "stream=sample_rate,channels",
                    "-of", "json", input_path,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            if probe.returncode == 0:
                streams = json.loads(
                    probe.stdout.decode("utf-8", errors="replace")
                ).get("streams", [])
                if streams:
                    sample_rate = int(streams[0].get("sample_rate", sample_rate))
                    channels = int(streams[0].get("channels", channels))
                    if sample_rate <= 0:
                        sample_rate = 44100
                    if channels <= 0 or channels > 6:
                        channels = 2
        except (OSError, ValueError, TypeError):
            pass

    command = command_prefix + [
        "-v", "error", "-i", input_path, "-map", "0:a:0",
        "-f", "s16le", "-acodec", "pcm_s16le",
        "-ar", str(sample_rate), "-ac", str(channels), "pipe:1",
    ]
    try:
        result = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
        )
    except OSError as exc:
        raise RuntimeError(f"Không thể khởi chạy FFmpeg: {exc}") from exc

    if result.returncode != 0:
        details = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(
            "FFmpeg không giải mã được tệp âm thanh."
            + (f"\nChi tiết: {details}" if details else "")
        )
    if not result.stdout:
        raise ValueError("Tệp không chứa luồng âm thanh có thể giải mã.")
    if len(result.stdout) % (channels * 2):
        raise ValueError("FFmpeg trả về dữ liệu PCM không đầy đủ.")
    return result.stdout, sample_rate, channels


# ===========================================================================
# 5. GIẢI MÃ ĐỊNH DẠNG .MI (MI01 DECODER)
# ===========================================================================
def decode_mi_file(mi_path):
    with open(mi_path, "rb") as f:
        hdr = f.read(32)
        if len(hdr) < 32:
            raise ValueError("Kích thước file .mi không hợp lệ (< 32 bytes)")

        magic, channels, bits_per_sample, sample_rate, codec, \
        block_frames, total_frames, uncompressed_size, \
        compressed_size, channel_mask = struct.unpack('<4sHH I HH I I I I', hdr)

        if magic != b'MI01':
            raise ValueError(f"Magic không khớp: {magic}, yêu cầu 'MI01'")

        payload = f.read()

    lane_nibbles = (block_frames // 2) - 1
    total_blocks = (total_frames + block_frames - 1) // block_frames
    plane_bytes = 8 + lane_nibbles
    block_bytes = plane_bytes * channels

    decoded_samples = [0] * (total_frames * channels)

    for blk in range(total_blocks):
        blk_offset = blk * block_bytes
        base_frame = blk * block_frames

        for ch in range(channels):
            plane_offset = blk_offset + ch * plane_bytes
            plane_data = payload[plane_offset : plane_offset + plane_bytes]
            if len(plane_data) < plane_bytes:
                break

            pred_a, idx_a_raw = struct.unpack('<hh', plane_data[0:4])
            pred_b, idx_b_raw = struct.unpack('<hh', plane_data[4:8])
            idx_a = min(max(idx_a_raw & 0xFF, 0), 88)
            idx_b = min(max(idx_b_raw & 0xFF, 0), 88)

            frame_a = base_frame
            if frame_a < total_frames:
                decoded_samples[frame_a * channels + ch] = pred_a

            frame_b = base_frame + (lane_nibbles + 1)
            if frame_b < total_frames:
                decoded_samples[frame_b * channels + ch] = pred_b

            nibbles = plane_data[8 : 8 + lane_nibbles]
            for i in range(lane_nibbles):
                byte_val = nibbles[i]
                code_a = byte_val & 0x0F
                code_b = byte_val >> 4

                pred_a = clamp16(pred_a + DELTA_TABLE[idx_a * 16 + code_a])
                idx_a = NEXT_STEP_TABLE[idx_a * 16 + code_a]
                curr_frame_a = base_frame + 1 + i
                if curr_frame_a < total_frames:
                    decoded_samples[curr_frame_a * channels + ch] = pred_a

                pred_b = clamp16(pred_b + DELTA_TABLE[idx_b * 16 + code_b])
                idx_b = NEXT_STEP_TABLE[idx_b * 16 + code_b]
                curr_frame_b = base_frame + (lane_nibbles + 1) + 1 + i
                if curr_frame_b < total_frames:
                    decoded_samples[curr_frame_b * channels + ch] = pred_b

    pcm_bytes = struct.pack(f'<{len(decoded_samples)}h', *decoded_samples)
    info = {
        "channels": channels,
        "sample_rate": sample_rate,
        "bits_per_sample": bits_per_sample,
        "total_frames": total_frames,
        "duration_sec": total_frames / sample_rate if sample_rate else 0,
        "compressed_size": len(hdr) + len(payload),
        "uncompressed_size": len(pcm_bytes),
        "savings_percent": (1.0 - (len(hdr) + len(payload)) / len(pcm_bytes)) * 100 if pcm_bytes else 0
    }
    return pcm_bytes, info


# ===========================================================================
# 6. TRÌNH PHÁT ÂM THANH RA LOA MÁY TÍNH WINDOWS VỚI ĐA LUỒNG & TIẾN TRÌNH LIVE
# ===========================================================================
def play_pcm_windows(pcm_bytes, sample_rate, channels, title="Audio"):
    """
    Phát trực tiếp ra loa qua winsound in-memory (0 ghi file ra đĩa).
    Dùng threading để phát nền mượt mà, đồng thời cập nhật thanh tiến trình.
    """
    stereo_pcm = mix_to_stereo(pcm_bytes, channels)
    pcm_len = len(stereo_pcm)

    header = bytearray(44)
    struct.pack_into(
        '<4sI4s4sIHHIIHH4sI', header, 0,
        b'RIFF', pcm_len + 36, b'WAVE',
        b'fmt ', 16, 1, 2, sample_rate,
        sample_rate * 4, 4, 16,
        b'data', pcm_len
    )
    in_memory_wav = bytes(header) + stereo_pcm
    duration_sec = (pcm_len // 4) / sample_rate if sample_rate else 0

    print(f"\n[audio] Playing: '{title}' ({sample_rate} Hz, Stereo)")
    print("        [Bấm Ctrl + C trong terminal để dừng phát]\n")

    def _play_worker():
        try:
            winsound.PlaySound(in_memory_wav, winsound.SND_MEMORY)
        except Exception:
            pass

    # Chạy phát âm thanh trong luồng nền an toàn (không bị chặn bới SND_ASYNC)
    player_thread = threading.Thread(target=_play_worker, daemon=True)
    player_thread.start()

    start_time = time.time()
    try:
        while player_thread.is_alive():
            elapsed = time.time() - start_time
            if duration_sec > 0:
                pct = min(100.0, (elapsed / duration_sec) * 100)
                bar_len = 28
                filled = int(bar_len * (pct / 100))
                bar = "█" * filled + "░" * (bar_len - filled)
                cur_m, cur_s = divmod(int(elapsed), 60)
                tot_m, tot_s = divmod(int(duration_sec), 60)
                sys.stdout.write(f"\r  [{cur_m:02d}:{cur_s:02d} / {tot_m:02d}:{tot_s:02d}] ▶ [{bar}] {pct:5.1f}%")
                sys.stdout.flush()
            time.sleep(0.1)

        tot_m, tot_s = divmod(int(duration_sec), 60)
        sys.stdout.write(f"\r  [{tot_m:02d}:{tot_s:02d} / {tot_m:02d}:{tot_s:02d}] ✓ [████████████████████████████] 100.0%\n")
        sys.stdout.flush()
        print("[audio] Playback complete.\n")
    except KeyboardInterrupt:
        winsound.PlaySound(None, winsound.SND_PURGE)
        print("\n\n[audio] Stopped by user (Ctrl+C).\n")


# ===========================================================================
# 7. CHƯƠNG TRÌNH CHÍNH (MAIN ENTRY POINT)
# ===========================================================================
def main():
    args = sys.argv[1:]

    # Bỏ qua tên binary nếu người dùng gõ: python play_mi.py MiMediaPlayer.okami <args>
    if args and ("MiMediaPlayer" in args[0] or args[0].endswith(".okami")):
        args = args[1:]

    if not args or args[0] in ("-h", "--help"):
        print("=" * 68)
        print("  Okami NT Media Player & Audio Tool (Windows Host Runner)")
        print("=" * 68)
        print("Cách sử dụng (Tương thích hoàn toàn với MiMediaPlayer.okami):")
        print("  python play_mi.py <file.mi>                    -> Phát file .mi ra loa Windows")
        print("  python play_mi.py <file.wav/mp3/raw/flac/...>  -> Tự động giải mã và phát ra loa")
        print("  python play_mi.py -c <input_audio> [output.mi] -> Đóng gói sang định dạng .mi")
        print("  python play_mi.py -b <audio1> [audio2 ...]     -> Đóng gói hàng loạt sang .mi")
        print("  python play_mi.py -d <file.mi> <output.raw>    -> Xuất dữ liệu PCM 16-bit thô")
        print("  python play_mi.py -i <file.mi>                 -> Xem thông số kỹ thuật file .mi")
        print("  python play_mi.py MiMediaPlayer.okami <args>  -> Chạy giả lập nhị phân Okami NT")
        return

    # Chế độ đóng gói hàng loạt; mỗi file .mi nằm cạnh file nguồn.
    if args[0] in ("-b", "--batch"):
        if len(args) < 2:
            print("Lỗi: Cần ít nhất một file đầu vào. Cách dùng: python play_mi.py -b <audio1> [audio2 ...]")
            raise SystemExit(2)
        failed = False
        for source in args[1:]:
            in_file = os.path.normpath(source)
            if os.path.splitext(in_file)[1].lower() == ".mi":
                print(f"[audio] Bỏ qua file đã ở định dạng .mi: '{in_file}'")
                continue
            out_mi = os.path.splitext(in_file)[0] + ".mi"
            try:
                print(f"[audio] Loading input: '{in_file}'")
                pcm, rate, ch = load_any_audio(in_file)
                frames, mi_size = encode_pcm_to_mi(pcm, rate, ch, out_mi)
                savings = (1.0 - mi_size / len(pcm)) * 100 if len(pcm) else 0
                print(f"[audio] Packed successfully! '{out_mi}' ({frames} frames, {mi_size:,} bytes, {savings:.1f}% space saved)")
            except (OSError, ValueError, RuntimeError, wave.Error) as exc:
                failed = True
                print(f"[audio] Lỗi với '{in_file}': {exc}")
        if failed:
            raise SystemExit(1)
        return

    # Chế độ đóng gói sang .mi (-c / --pack)
    if args[0] in ("-c", "--pack"):
        if len(args) < 2:
            print("Lỗi: Thiếu file đầu vào. Cách dùng: python play_mi.py -c <input_audio> [out.mi]")
            return
        in_file = os.path.normpath(args[1])
        out_mi = os.path.normpath(args[2]) if len(args) >= 3 else os.path.splitext(in_file)[0] + ".mi"
        print(f"[audio] Loading input: '{in_file}'")
        pcm, rate, ch = load_any_audio(in_file)
        print(f"[audio] Detected: {rate} Hz, {ch} ch, 16-bit")
        print(f"[audio] Encoding to .mi -> '{out_mi}'...")
        frames, mi_size = encode_pcm_to_mi(pcm, rate, ch, out_mi)
        savings = (1.0 - mi_size / len(pcm)) * 100 if len(pcm) else 0
        print(f"[audio] Packed successfully! ({frames} frames, {mi_size:,} bytes, {savings:.1f}% space saved)")
        return

    # Chế độ xuất dữ liệu PCM thô (-d / --decode)
    if args[0] in ("-d", "--decode"):
        if len(args) < 3:
            print("Lỗi: Thiếu đường dẫn file. Cách dùng: python play_mi.py -d <file.mi> <out.raw>")
            return
        mi_path = os.path.normpath(args[1])
        out_raw = os.path.normpath(args[2])
        pcm_bytes, info = decode_mi_file(mi_path)
        with open(out_raw, "wb") as f:
            f.write(pcm_bytes)
        print(f"[audio] Exported raw PCM -> '{out_raw}' ({len(pcm_bytes):,} bytes, {info['channels']} ch, {info['sample_rate']} Hz)")
        return

    # Chế độ xem thông tin (-i / --info)
    if args[0] in ("-i", "--info"):
        if len(args) < 2:
            print("Lỗi: Thiếu file .mi. Cách dùng: python play_mi.py -i <file.mi>")
            return
        mi_path = os.path.normpath(args[1])
        _, info = decode_mi_file(mi_path)
        ch_name = "Mono (1 kênh)" if info['channels'] == 1 else ("Stereo (2 kênh)" if info['channels'] == 2 else f"{info['channels']} kênh")
        print(f"File: {os.path.basename(mi_path)}")
        print(f"Format: MI01 Okami ADPCM 4:1")
        print(f"Rate: {info['sample_rate']} Hz | Channels: {ch_name} | Duration: {info['duration_sec']:.2f} s")
        print(f"Size: {info['compressed_size']:,} bytes (PCM gốc: {info['uncompressed_size']:,} bytes, Tiết kiệm: {info['savings_percent']:.1f}%)")
        return

    # Mặc định: Phát âm thanh
    file_path = os.path.normpath(args[0])
    if not os.path.exists(file_path):
        print(f"Lỗi: Không tìm thấy file '{file_path}'")
        return

    ext = os.path.splitext(file_path)[1].lower()

    if ext == ".mi":
        # Phát file .mi
        print(f"[audio] Opening .mi: '{file_path}'")
        pcm_bytes, info = decode_mi_file(file_path)
        play_pcm_windows(pcm_bytes, info['sample_rate'], info['channels'], title=os.path.basename(file_path))
    else:
        # Tự động nạp và phát mọi định dạng âm thanh (.wav, .mp3, .raw, .flac,...)
        print(f"[audio] Opening audio: '{file_path}'")
        pcm_bytes, rate, channels = load_any_audio(file_path)
        play_pcm_windows(pcm_bytes, rate, channels, title=os.path.basename(file_path))


if __name__ == '__main__':
    main()
