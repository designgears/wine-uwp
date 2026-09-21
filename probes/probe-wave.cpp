// probe-wave: play a .wav through waveOut, to prove the prefix can reach a host audio device at
// all before anything is built on top of it. Reports the format it found and how long it played.
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>

static bool read_all(const wchar_t *path, BYTE **data, DWORD *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { wprintf(L"open %ls failed: %lu\n", path, GetLastError()); return false; }
    *size = GetFileSize(f, nullptr);
    *data = (BYTE *)malloc(*size);
    DWORD read = 0;
    ReadFile(f, *data, *size, &read, nullptr);
    CloseHandle(f);
    return read == *size;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) { wprintf(L"usage: probe-wave <file.wav>\n"); return 2; }

    BYTE *data; DWORD size;
    if (!read_all(argv[1], &data, &size)) return 1;
    if (size < 44 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4))
    { wprintf(L"not a RIFF/WAVE file\n"); return 1; }

    // walk the chunks for "fmt " and "data"
    WAVEFORMATEX *fmt = nullptr; BYTE *pcm = nullptr; DWORD pcm_size = 0;
    for (DWORD off = 12; off + 8 <= size; )
    {
        DWORD chunk = *(DWORD *)(data + off + 4);
        if (!memcmp(data + off, "fmt ", 4)) fmt = (WAVEFORMATEX *)(data + off + 8);
        else if (!memcmp(data + off, "data", 4)) { pcm = data + off + 8; pcm_size = chunk; }
        off += 8 + chunk + (chunk & 1);
    }
    if (!fmt || !pcm) { wprintf(L"no fmt/data chunk\n"); return 1; }
    wprintf(L"format tag %u, %u ch, %lu Hz, %u bits, %lu bytes of PCM\n",
            fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec, fmt->wBitsPerSample, pcm_size);

    wprintf(L"waveOutGetNumDevs: %u\n", waveOutGetNumDevs());

    HWAVEOUT out = nullptr;
    MMRESULT mr = waveOutOpen(&out, WAVE_MAPPER, fmt, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) { wprintf(L"waveOutOpen failed: %u\n", mr); return 1; }

    WAVEHDR hdr = {};
    hdr.lpData = (LPSTR)pcm;
    hdr.dwBufferLength = pcm_size;
    if ((mr = waveOutPrepareHeader(out, &hdr, sizeof(hdr))) != MMSYSERR_NOERROR)
    { wprintf(L"prepare failed: %u\n", mr); return 1; }
    if ((mr = waveOutWrite(out, &hdr, sizeof(hdr))) != MMSYSERR_NOERROR)
    { wprintf(L"write failed: %u\n", mr); return 1; }

    DWORD start = GetTickCount();
    while (!(hdr.dwFlags & WHDR_DONE) && GetTickCount() - start < 30000) Sleep(50);
    wprintf(L"played for %lu ms (done=%d)\n", GetTickCount() - start, !!(hdr.dwFlags & WHDR_DONE));

    waveOutUnprepareHeader(out, &hdr, sizeof(hdr));
    waveOutClose(out);
    return 0;
}
