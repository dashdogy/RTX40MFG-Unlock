// Included inside ampere_gpu's private namespace. The payload is replaced only
// while the host owns a live first-initialization preparation ticket.
constexpr size_t kFontBytes = 6752;
struct FontProgram
{
    HMODULE module = nullptr;
    uintptr_t address = 0;
    size_t pageBytes = 0, pageCount = 0;
    uintptr_t pageStart = 0;
    const char* stage = "image";
    std::array<DWORD, 3> protections{};
    std::array<uint8_t, kFontBytes> original{}, replacement{};
};
bool FontCurrent(const FontProgram&, bool replacement) noexcept;
bool PrepareFont(HMODULE, uint32_t imageSize, FontProgram&,
    std::vector<uint8_t>& ptx, std::vector<uint8_t>& native);
bool WriteFont(FontProgram&, bool replacement,
    protected_pointer::ProtectMemoryFn protect) noexcept;
bool RestoreFontPage(const FontProgram&, size_t page,
    protected_pointer::ProtectMemoryFn protect) noexcept;
