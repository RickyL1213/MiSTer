#include "n64.h"
#include "n64_cpak_header.h"
#include "../../menu.h"
#include "../../user_io.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "lib/md5/md5.h"

static constexpr auto CARTID_LENGTH = 6U; // Ex: NSME00
static constexpr auto MD5_LENGTH = 16U;
static constexpr auto CARTID_PREFIX = "ID:";
static constexpr auto AR_TYPE_OPT = "[48:47]";
static constexpr auto AUTODETECT_OPT = "[64]";
static constexpr auto CIC_TYPE_OPT = "[68:65]";
static constexpr auto NO_EPAK_OPT = "[70]";
static constexpr auto CPAK_OPT = "[71]";
static constexpr auto RPAK_OPT = "[72]";
static constexpr auto TPAK_OPT = "[73]";
static constexpr auto RTC_OPT = "[74]";
static constexpr auto SAVE_TYPE_OPT = "[77:75]";
static constexpr auto SYS_TYPE_OPT = "[80:79]";
static constexpr const char *const CONTROLLER_OPTS[] = { "[51:49]", "[54:52]", "[57:55]", "[60:58]" };

// Simple hash function, see: https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
// (Modified to make it case-insensitive)
static constexpr uint64_t FNV_PRIME = 0x100000001b3;
static constexpr uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325;
static constexpr uint64_t fnv_hash(const char* s, uint64_t h = FNV_OFFSET_BASIS) {
	if (s) while (uint8_t a = *(s++)) h = (h ^ ((a >= 'A') && (a <= 'Z') ? a + ('a' - 'A') : a)) * FNV_PRIME;
	return h;
}

enum class MemoryType : uint32_t {
	NONE = 0,
	EEPROM_512,
	EEPROM_2k,
	SRAM_32k,
	SRAM_96k,
	FLASH_128k,
	CPAK = ~2U,
	TPAK = ~1U,
	UNKNOWN = ~0U
};

enum class CIC : uint32_t {
	CIC_NUS_6101 = 0,
	CIC_NUS_6102,
	CIC_NUS_7101,
	CIC_NUS_7102,
	CIC_NUS_6103,
	CIC_NUS_7103,
	CIC_NUS_6105,
	CIC_NUS_7105,
	CIC_NUS_6106,
	CIC_NUS_7106,
	CIC_NUS_8303,
	CIC_NUS_8401,
	CIC_NUS_5167,
	CIC_NUS_DDUS,
	CIC_NUS_5101,
	UNKNOWN = ~0U
};

enum class SystemType : uint32_t {
	NTSC = 0,
	PAL,
	UNKNOWN = ~0U
};

enum class DataFormat : uint32_t {
	BIG_ENDIAN = 0,
	BYTE_SWAPPED,
	LITTLE_ENDIAN,
	UNKNOWN = ~0U
};

enum class PadType : uint32_t {
	N64_PAD = 0,
	UNPLUGGED,
	N64_PAD_WITH_CPAK,
	N64_PAD_WITH_RPAK,
	SNAC,
	N64_PAD_WITH_TPAK,
	UNKNOWN = ~0U
};

enum class AutoDetect : uint32_t {
	ON = 0,
	OFF
};

enum class AspectRatio : uint32_t {
	ORIGINAL = 0,
	FULL,
	CUSTOM_1,
	CUSTOM_2,
	UNKNOWN = ~0U
};

static AspectRatio old_ar = AspectRatio::UNKNOWN;

static const char* stringify(MemoryType v) {
	switch (v) {
		case MemoryType::EEPROM_512: return "4K EEPROM";
		case MemoryType::EEPROM_2k: return "16K EEPROM";
		case MemoryType::SRAM_32k: return "256K SRAM";
		case MemoryType::SRAM_96k: return "768K SRAM";
		case MemoryType::FLASH_128k: return "Flash RAM";
		case MemoryType::CPAK: return "CPAK DATA";
		case MemoryType::TPAK: return "TPAK DATA";
		default: return "(none)";
	}
}

static size_t get_save_size(MemoryType v) {
	switch (v) {
		case MemoryType::EEPROM_512: return 0x200;
		case MemoryType::EEPROM_2k: return 0x800;
		case MemoryType::SRAM_32k: return 0x8000;
		case MemoryType::SRAM_96k: return 0x18000;
		case MemoryType::FLASH_128k: return 0x20000;
		case MemoryType::CPAK:
		case MemoryType::TPAK: return 0x8000; // 32 kByte
		default: return 0;
	}
}

static const char* stringify(CIC v) {
	switch (v) {
		case CIC::CIC_NUS_6101: return "6101";
		case CIC::CIC_NUS_6102: return "6102";
		case CIC::CIC_NUS_7101: return "7101";
		case CIC::CIC_NUS_7102: return "7102";
		case CIC::CIC_NUS_6103: return "6103";
		case CIC::CIC_NUS_7103: return "7103";
		case CIC::CIC_NUS_6105: return "6105";
		case CIC::CIC_NUS_7105: return "7105";
		case CIC::CIC_NUS_6106: return "6106";
		case CIC::CIC_NUS_7106: return "7106";
		case CIC::CIC_NUS_8303: return "8303";
		case CIC::CIC_NUS_8401: return "8401";
		case CIC::CIC_NUS_5167: return "5167";
		case CIC::CIC_NUS_DDUS: return "DDUS";
		case CIC::CIC_NUS_5101: return "5101";
		default: return "Unknown";
	}
}

static const char* stringify(SystemType v) {
	switch (v) {
		case SystemType::NTSC: return "NTSC";
		case SystemType::PAL: return "PAL";
		default: return "Unknown";
	}
}

static const char* stringify(bool v) {
	return v 
		? "Yes" 
		: "No";
}

static DataFormat detectRomFormat(const uint8_t *data) {
	// Data should be aligned
	const uint32_t val = *(uint32_t *)data;

	/* The following checks assume we're on a little-endian platform.
	   For each check, the first value is for regular ROMs, the 2nd is for 64DD images and
	   the third is a malformed "word" used in some homebrew(?) */
	switch (val) {
		case UINT32_C(0x40123780):
		case UINT32_C(0x40072780):
		case UINT32_C(0x41123780):
			return DataFormat::BIG_ENDIAN;
		case UINT32_C(0x12408037):
		case UINT32_C(0x07408027):
		case UINT32_C(0x12418037):
			return DataFormat::BYTE_SWAPPED;
		case UINT32_C(0x80371240):
		case UINT32_C(0x80270740):
		case UINT32_C(0x80371241):
			return DataFormat::LITTLE_ENDIAN;
		default:
			break;
	}

	// Endianness could not be determined, use just first byte.
	switch (val & 0xff) {
		case 0x80:
			return DataFormat::BIG_ENDIAN;
		case 0x37:
		case 0x27:
			return DataFormat::BYTE_SWAPPED;
		case 0x40:
		case 0x41:
			return DataFormat::LITTLE_ENDIAN;
		default:
			return DataFormat::UNKNOWN;
	}
}

static void normalizeData(uint8_t* data, size_t size, DataFormat format) {
	switch (format) {
		case DataFormat::BYTE_SWAPPED:
			size &= ~1U;
			for (size_t i = 0; i < size; i += 2) {
				auto c0 = data[0];
				auto c1 = data[1];
				data[0] = c1;
				data[1] = c0;
				data += 2;
			}
			break;
		case DataFormat::LITTLE_ENDIAN:
			size &= ~3U;
			for (size_t i = 0; i < size; i += 4) {
				auto c0 = data[0];
				auto c1 = data[1];
				auto c2 = data[2];
				auto c3 = data[3];
				data[0] = c3;
				data[1] = c2;
				data[2] = c1;
				data[3] = c0;
				data += 4;
			}
			break;
		default:
			// Nothing to do
			break;
	}
}

static MemoryType get_cart_save_type() {
	auto v = (MemoryType)user_io_status_get(SAVE_TYPE_OPT);
	return (get_save_size(v) ? v : MemoryType::NONE);
}

static void set_cart_save_type(MemoryType v) {
	user_io_status_set(SAVE_TYPE_OPT,
		(uint32_t)(get_save_size(v) ? v : MemoryType::NONE));
}

static uint32_t get_save_file_offset(unsigned char idx) {
	uint32_t offset = 0;
	MemoryType save_type = get_cart_save_type();

	if (idx && (save_type != MemoryType::NONE)) {
		offset += get_save_size(save_type);
		idx--;
	}

	if (idx && (bool)user_io_status_get(TPAK_OPT)) {
		offset += get_save_size(MemoryType::TPAK);
		idx--;
	}

	offset += get_save_size(MemoryType::CPAK) * idx;

	return offset;
}

static char full_path[1024];

static size_t create_file(const char* filename, const uint8_t* data, size_t sz) {
	sprintf(full_path, "%s/%s", getRootDir(), filename);
	printf("Open file %s\n", full_path);
	FILE* fp = fopen(full_path, "w");
	if (!fp) {
		return 0;
	}
	sz = fwrite(data, sizeof(uint8_t), sz, fp);
	printf("Wrote %u bytes\n", sz);
	fclose(fp);
	return sz;
}

static size_t read_file(const char* filename, uint8_t* data, uint32_t offset, size_t sz) {
	sprintf(full_path, "%s/%s", getRootDir(), filename);
	printf("Open file %s\n", full_path);
	FILE* fp = fopen(full_path, "r");
	if (!fp) {
		return 0;
	}
	fseek(fp, 0L, SEEK_END);
	size_t fs = ftell(fp);
	printf("File is %u bytes\n", fs);
	if (offset > fs) {
		return 0;
	}
	if (offset + sz > fs) {
		sz = fs - offset;
	}
	fseek(fp, offset, SEEK_SET);
	printf("Reading %u to %u\n", offset, offset + sz);
	sz = fread(data, sizeof(uint8_t), sz, fp);
	printf("Read %u bytes\n", sz);
	fclose(fp);
	return sz;
}

static uint8_t mounted_save_files;
static uint8_t save_file_buf[0x20000]; // Largest save size

struct SaveFile {
	int idx = -1;
	MemoryType type;

	const char* get_path() {
		if (idx < 0) {
			return nullptr;
		}
		auto file = get_image(idx);
		if (!file) {
			return nullptr;
		}
		return file->name;
	}

	size_t get_size() {
		return get_save_size(type);
	}

	void mount(const char* path) {
		idx = mounted_save_files++;
		user_io_file_mount(path, idx, 1);
	}

	// Return "true" if created
	bool create_if_missing(const char* path, const char* old_path) {
		if (FileExists(path, 0)) {
			return false;
		}

		auto sz = get_size();
		memset(save_file_buf, 0, sz);
		bool found_old_data = false;

		if (sz && FileExists(old_path, 0)) {
			uint32_t off = get_save_file_offset((idx < 0) ? mounted_save_files : idx);
			if (read_file(old_path, save_file_buf, off, sz)) {
				printf("Found old save data (%s), converting to %s.\n", old_path, stringify(type));
				found_old_data = true;
				if ((type == MemoryType::CPAK) || (type == MemoryType::TPAK)) {
					normalizeData(save_file_buf, sz, DataFormat::LITTLE_ENDIAN);
				}
			}
		}

		if (!found_old_data && (type == MemoryType::CPAK)) {
			memcpy(save_file_buf, cpak_header[((idx < 0) ? mounted_save_files : idx) % (sizeof(cpak_header) / sizeof(*cpak_header))], sizeof(*cpak_header));
		}

		if (!create_file(path, save_file_buf, sz)) {
			return false;
		}

		printf("Created file: %s (%u bytes)\n", path, sz);
		return true;
	}

	SaveFile(MemoryType type) {
		this->type = type;
	}

	SaveFile() {
		this->type = MemoryType::NONE;
	}
};

static SaveFile save_files[8]{};

static bool is_auto() {
	return (AutoDetect)user_io_status_get(AUTODETECT_OPT) == AutoDetect::ON;
}

static uint8_t hex_to_dec(const char x) {
	if (x >= '0' && x <= '9') return (x - '0');
	if (x >= 'A' && x <= 'F') return (x - 'A') + 10;
	if (x >= 'a' && x <= 'f') return (x - 'a') + 10;
	return 0;
}

static void trim(char* out, size_t len, const char* str)
{
	if (len == 0) {
		return;
	}

	const char* end;
	size_t out_size;

	// Trim leading space
	while (isspace((unsigned char)*str)) {
		str++;
	}

	// All spaces?
	if (*str == '\0') {
		*out = '\0';
		return;
	}

	// Trim trailing space
	end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char)*end)) {
		end--;
	}
	end++;

	// Set output size to minimum of trimmed string length and buffer size minus 1
	out_size = (size_t)(end - str) < (len - 1) ? (end - str) : len - 1;

	// Copy trimmed string and add null terminator
	memcpy(out, str, out_size);
	out[out_size] = '\0';

	// Obfuscate illegal characters
	for (size_t i = 0; (i < out_size) && out[i]; i++) {
		if ((out[i] >= 0x20) && (out[i] < 0xa0)) {
			continue;
		}
		out[i] = '?';
	}
}

// Returns true if CIC and System Region is detected, or if auto-detection is turned off
static bool parse_and_apply_db_tags(char *tags) {
	if (tags == nullptr) return false;

	const char* separator = "| ";
	MemoryType save_type = MemoryType::NONE;
	SystemType system_type = SystemType::UNKNOWN;
	CIC cic_type = CIC::UNKNOWN;
	bool no_epak = false;
	bool cpak = false;
	bool rpak = false;
	bool tpak = false;
	bool rtc = false;
	bool wide = false;

	PadType prefered_pad = PadType::N64_PAD;

	for (auto tag = strtok(tags, separator); tag; tag = strtok(nullptr, separator)) {
		switch (fnv_hash(tag)) {
			case fnv_hash("eeprom512"): save_type = MemoryType::EEPROM_512; break;
			case fnv_hash("eeprom2k"): save_type = MemoryType::EEPROM_2k; break;
			case fnv_hash("sram32k"): save_type = MemoryType::SRAM_32k; break;
			case fnv_hash("sram96k"): save_type = MemoryType::SRAM_96k; break;
			case fnv_hash("flash128k"): save_type = MemoryType::FLASH_128k; break;
			case fnv_hash("noepak"): no_epak = true; break;
			case fnv_hash("cpak"): cpak = true; if (prefered_pad == PadType::N64_PAD) prefered_pad = PadType::N64_PAD_WITH_CPAK; break;
			case fnv_hash("rpak"): rpak = true; if (prefered_pad == PadType::N64_PAD) prefered_pad = PadType::N64_PAD_WITH_RPAK; break;
			case fnv_hash("tpak"): tpak = true; if (prefered_pad == PadType::N64_PAD) prefered_pad = PadType::N64_PAD_WITH_TPAK; break;
			case fnv_hash("rtc"): rtc = true; break;
			case fnv_hash("ntsc"): ntsc: system_type = SystemType::NTSC; break;
			case fnv_hash("pal"): pal: system_type = SystemType::PAL; break;
			case fnv_hash("wide"): wide = true; break;
			case fnv_hash("cic6101"): cic_type = CIC::CIC_NUS_6101; goto ntsc;
			case fnv_hash("cic6102"): cic_type = CIC::CIC_NUS_6102; goto ntsc;
			case fnv_hash("cic6103"): cic_type = CIC::CIC_NUS_6103; goto ntsc;
			case fnv_hash("cic6105"): cic_type = CIC::CIC_NUS_6105; goto ntsc;
			case fnv_hash("cic6106"): cic_type = CIC::CIC_NUS_6106; goto ntsc;
			case fnv_hash("cic7101"): cic_type = CIC::CIC_NUS_7101; goto pal;
			case fnv_hash("cic7102"): cic_type = CIC::CIC_NUS_7102; goto pal;
			case fnv_hash("cic7103"): cic_type = CIC::CIC_NUS_7103; goto pal;
			case fnv_hash("cic7105"): cic_type = CIC::CIC_NUS_7105; goto pal;
			case fnv_hash("cic7106"): cic_type = CIC::CIC_NUS_7106; goto pal;
			case fnv_hash("cic8303"): cic_type = CIC::CIC_NUS_8303; break;
			case fnv_hash("cic8401"): cic_type = CIC::CIC_NUS_8401; break;
			case fnv_hash("cic5167"): cic_type = CIC::CIC_NUS_5167; break;
			case fnv_hash("cicddus"): cic_type = CIC::CIC_NUS_DDUS; break;
			case fnv_hash("cic5101"): cic_type = CIC::CIC_NUS_5101; break;
			default: printf("Unknown tag: [%s] (skipping)\n", tag); break;
		}
	}

	if (system_type == SystemType::UNKNOWN && cic_type != CIC::UNKNOWN) {
		system_type = SystemType::NTSC;
	}

	printf("System: %s, Save Type: %s, CIC: %s, CPak: %s, RPak: %s, TPak %s, RTC: %s, Mem: %uMB\n",
		stringify(system_type),
		stringify(save_type),
		stringify(cic_type),
		stringify(cpak),
		stringify(rpak),
		stringify(tpak),
		stringify(rtc),
		(no_epak ? 4 : 8));

	if (!is_auto()) {
		printf("Auto-detect is OFF, not updating OSD settings\n");
		return true;
	}

	printf("Auto-detect is ON, updating OSD settings\n");

	if (system_type != SystemType::UNKNOWN) user_io_status_set(SYS_TYPE_OPT, (uint32_t)system_type);
	if (cic_type != CIC::UNKNOWN) user_io_status_set(CIC_TYPE_OPT, (uint32_t)cic_type);

	user_io_status_set(NO_EPAK_OPT, (uint32_t)no_epak);
	user_io_status_set(CPAK_OPT, (uint32_t)cpak);
	user_io_status_set(RPAK_OPT, (uint32_t)rpak);
	user_io_status_set(TPAK_OPT, (uint32_t)tpak);
	user_io_status_set(RTC_OPT, (uint32_t)rtc);
	set_cart_save_type(save_type);

	if ((PadType)user_io_status_get(CONTROLLER_OPTS[0]) != PadType::SNAC) {
		user_io_status_set(CONTROLLER_OPTS[0], (uint32_t)prefered_pad);
	}

	auto current_ar = (AspectRatio)user_io_status_get(AR_TYPE_OPT);
	if (wide) {
		if (current_ar != AspectRatio::FULL) {
			old_ar = current_ar;
		}
		user_io_status_set(AR_TYPE_OPT, (uint32_t)AspectRatio::FULL);
	}
	else if ((current_ar == AspectRatio::FULL) && (old_ar != AspectRatio::UNKNOWN)) {
		user_io_status_set(AR_TYPE_OPT, (uint32_t)old_ar);
		old_ar = AspectRatio::UNKNOWN;
	}

	return (system_type != SystemType::UNKNOWN && cic_type != CIC::UNKNOWN);
}

static bool md5_matches(const char* line, const char* md5) {
	for (size_t i = MD5_LENGTH * 2; i > 0; --i, line++, md5++) {
		int c = *line;
		if (!c || *md5 != tolower(c)) {
			return false;
		}
	}

	return true;
}

// Returns numbers of matching characters if match, otherwize 0
static size_t cart_id_is_match(const char* line, const char* cart_id) {
	const auto prefix_len = strlen(CARTID_PREFIX);

	// A valid ID line should start with "ID:"
	if (strncmp(line, CARTID_PREFIX, prefix_len) != 0) {
		return 0;
	}

	// Skip the line if it doesn't match our cart_id, '_' = don't care
	const char* lp = line + prefix_len;
	for (size_t i = 0; i < CARTID_LENGTH && *lp; i++, lp++) {
		if (i && isspace(*lp)) {
			return i; // Early termination
		}

		if (*lp != '_' && *lp != cart_id[i]) {
			return 0; // Character didn't match pattern
		}
	}

	return CARTID_LENGTH;
}

static uint8_t detect_rom_settings_in_db(const char* lookup_hash, const char* db_file_name) {
	fileTextReader reader = {};

	char file_path[1024];
	sprintf(file_path, "%s/%s", HomeDir(), db_file_name);

	if (!FileOpenTextReader(&reader, file_path)) {
		printf("Failed to open N64 data file %s\n", file_path);
		return 0;
	}

	while (const char* line = FileReadLine(&reader)) {
		// Skip the line if it doesn't start with our hash
		if (!md5_matches(line, lookup_hash)) {
			continue;
		}

		const char* s = line + (MD5_LENGTH * 2);
		char* tags = new char[strlen(s) + 1];
		if (sscanf(s, "%*[ \t]%[^#;]", tags) <= 0) {
			printf("Found ROM entry for MD5 [%s], but the tag was malformed! (%s)\n", lookup_hash, s);
			return 2;
		}

		printf("Found ROM entry for MD5 [%s]: %s\n", lookup_hash, tags);

		// 2 = System region and/or CIC wasn't in DB, will need further detection
		return parse_and_apply_db_tags(tags) ? 3 : 2;
	}

	return 0;
}

static uint8_t detect_rom_settings_in_db_with_cartid(const char* cart_id, const char* db_file_name) {
	fileTextReader reader = {};

	char file_path[1024];
	sprintf(file_path, "%s/%s", HomeDir(), db_file_name);

	if (!FileOpenTextReader(&reader, file_path)) {
		printf("Failed to open N64 data file %s\n", file_path);
		return 0;
	}

	size_t i;

	while (const char* line = FileReadLine(&reader)) {
		// Skip lines that doesn't start with our ID
		if (!(i = cart_id_is_match(line, cart_id))) continue;

		auto s = line + strlen(CARTID_PREFIX) + i;
		auto tags = new char[strlen(s) + 1];
		if (sscanf(s, "%*[ \t]%[^#;]", tags) <= 0) {
			printf("Found ROM entry for ID [%s], but the tag was malformed! (%s)\n", cart_id, s);
			return 2;
		}

		printf("Found ROM entry for ID [%s]: %s\n", cart_id, tags);

		// 2 = System region and/or CIC wasn't in DB, will need further detection
		return parse_and_apply_db_tags(tags) ? 3 : 2;
	}

	return 0;
}

static const char* DB_FILE_NAMES[] = {
	"N64-database_user.txt",
	"N64-database.txt"
};

static uint8_t detect_rom_settings_in_dbs_with_md5(const char* lookup_hash) {
	uint8_t detected = 0;
	for (auto i = 0U; i < (sizeof(DB_FILE_NAMES) / sizeof(*DB_FILE_NAMES)); i++) {
		if ((detected = detect_rom_settings_in_db(lookup_hash, DB_FILE_NAMES[i]))) {
			break;
		}
	}

	return detected;
}

static uint8_t detect_rom_settings_in_dbs_with_cartid(const char* lookup_id) {
	// Check if all characters in the lookup are valid
	for (size_t i = 0; i < CARTID_LENGTH; i++) {
		auto c = lookup_id[i];
		if (isalnum(c)) {
			continue;
		}

		printf("Not a valid Cart ID: [%s]!\n", lookup_id);
		return 0;
	}

	uint8_t detected = 0;
	for (auto i = 0U; i < (sizeof(DB_FILE_NAMES) / sizeof(*DB_FILE_NAMES)); i++) {
		if ((detected = detect_rom_settings_in_db_with_cartid(lookup_id, DB_FILE_NAMES[i]))) {
			break;
		}
	}

	return detected;
}

// "Advanced" Homebrew ROM Header https://n64brew.dev/wiki/ROM_Header
static bool detect_homebrew_header(const uint8_t* controller_settings, const char* cart_id) {
	if ((cart_id[1] != 'E') || (cart_id[2] != 'D')) {
		return false;
	}

	printf("Detected Advanced Homebrew ROM Header, how fancy!\n");

	if (!is_auto()) {
		printf("Auto-detect is OFF, not updating OSD settings\n");
		return false;
	}

	switch (hex_to_dec(cart_id[4])) {
		case 1:
			set_cart_save_type(MemoryType::EEPROM_512);
			break;
		case 2:
			set_cart_save_type(MemoryType::EEPROM_2k);
			break;
		case 3:
			set_cart_save_type(MemoryType::SRAM_32k);
			break;
		case 4:
			set_cart_save_type(MemoryType::SRAM_96k);
			break;
		case 5:
			set_cart_save_type(MemoryType::FLASH_128k);
			break;
		//case 6:
		//	set_cart_save_type(MemoryType::SRAM_128k);
		//	break;
		default:
			set_cart_save_type(MemoryType::NONE);
			break;
	}

	printf("Auto-detect is ON, updating OSD settings\n");

	user_io_status_set(RTC_OPT, (uint32_t)(hex_to_dec(cart_id[5]) & 1)); // RTC

	user_io_status_set(RPAK_OPT, (uint32_t)(
		(controller_settings[0]) == 0x01 ||
		(controller_settings[1]) == 0x01 ||
		(controller_settings[2]) == 0x01 ||
		(controller_settings[3]) == 0x01 ? 1 : 0)); // Rumble Pak

	user_io_status_set(CPAK_OPT, (uint32_t)(
		(controller_settings[0] == 0x02) ||
		(controller_settings[1] == 0x02) ||
		(controller_settings[2] == 0x02) ||
		(controller_settings[3] == 0x02) ? 1 : 0)); // Controller Pak

	user_io_status_set(TPAK_OPT, (uint32_t)(
		(controller_settings[0] == 0x03) ||
		(controller_settings[1] == 0x03) ||
		(controller_settings[2] == 0x03) ||
		(controller_settings[3] == 0x03) ? 1 : 0)); // Transfer Pak

	size_t c_idx = 0;
	for (auto c_opt : CONTROLLER_OPTS) {
		if (controller_settings[c_idx] && ((PadType)user_io_status_get(c_opt) != PadType::SNAC)) {
			if (controller_settings[c_idx] < 0x80) {
				user_io_status_set(c_opt, (uint32_t)(
					(controller_settings[c_idx] == 0x01) ? PadType::N64_PAD_WITH_RPAK :
					(controller_settings[c_idx] == 0x02) ? PadType::N64_PAD_WITH_CPAK :
					(controller_settings[c_idx] == 0x03) && (c_idx == 0) ? PadType::N64_PAD_WITH_TPAK :
					PadType::N64_PAD));
			}
			else if (controller_settings[c_idx] == 0xff) {
				user_io_status_set(c_opt, (uint32_t)PadType::UNPLUGGED);
			}
		}
		c_idx++;
	}

	return true;
}

static bool detect_rom_settings_from_first_chunk(const char region_code, const uint64_t *signatures) {
	SystemType system_type;
	CIC cic = CIC::UNKNOWN;
	bool is_known_cic = true;

	switch (region_code) {
		case 'D': // Germany
		case 'F': // France
		case 'H': // Netherlands
		case 'I': // Italy
		case 'L': // Gateway 64 (PAL)
		case 'P': // Europe
		case 'S': // Spain
		case 'U': // Australia
		case 'W': // Scandinavia
		case 'X': // Europe
		case 'Y': // Europe
		case 'Z': // Europe
			system_type = SystemType::PAL; break;
		default:
			system_type = SystemType::NTSC; break;
	}

	// How many signatures are we checking against? (Normal and Aleck64)
	size_t n = 2;

	if (n) do {
		switch (*signatures) {
			default:
				if (--n) {
					printf("Unknown CIC (Signature: 0x%016llx), tries next.\n", *signatures);
					signatures++;
					break;
				}
				printf("Unknown CIC (Signature: 0x%016llx), uses default.\n", *signatures);
				is_known_cic = false;
				// Fall through
			case UINT64_C(0x000000a316adc55a): // CIC-6102/7101 IPL3
			case UINT64_C(0x000000a30dacd530): // NOP:ed out CRC check
			case UINT64_C(0x000000039c981107): // hcs64's CIC-6102 IPL3 replacement
			case UINT64_C(0x000000d2828281b0): // Unknown. Used in some homebrew
			case UINT64_C(0x000000d2be3c4486): // Xeno Crisis custom IPL3
			case UINT64_C(0x0000009acc31e644): // HW1 IPL3 (Turok E3 prototype)
			case UINT64_C(0x0000009474732e6b): // IPL3 re-assembled with the GNU assembler (iQue)
				cic = (system_type != SystemType::PAL) ? CIC::CIC_NUS_6102 : CIC::CIC_NUS_7101; break;
			case UINT64_C(0x000000a405397b05): // CIC-7102 IPL3
			case UINT64_C(0x000000a3fc388adb): // NOP:ed out CRC check
				system_type = SystemType::PAL; cic = CIC::CIC_NUS_7102; break;
			case UINT64_C(0x000000a0f26f62fe): // CIC-6101 IPL3
			case UINT64_C(0x000000a0e96e72d4): // NOP:ed out CRC check
				system_type = SystemType::NTSC; cic = CIC::CIC_NUS_6101; break;
			case UINT64_C(0x000000a9229d7c45): // CIC-x103 IPL3
			case UINT64_C(0x000000a9199c8c1b): // NOP:ed out CRC check
			case UINT64_C(0x000000271316d406): // All zeros bar font (iQue Paper Mario)
				cic = (system_type != SystemType::PAL) ? CIC::CIC_NUS_6103 : CIC::CIC_NUS_7103; break;
			case UINT64_C(0x000000f8b860ed00): // CIC-x105 IPL3
			case UINT64_C(0x000000f8af5ffcd6): // NOP:ed out CRC check
				cic = (system_type != SystemType::PAL) ? CIC::CIC_NUS_6105 : CIC::CIC_NUS_7105; break;
			case UINT64_C(0x000000ba5ba4b8cd): // CIC-x106 IPL3
				cic = (system_type != SystemType::PAL) ? CIC::CIC_NUS_6106 : CIC::CIC_NUS_7106; break;
			case UINT64_C(0x0000012daafc8aab): cic = CIC::CIC_NUS_5167; break;
			case UINT64_C(0x000000a9df4b39e1): cic = CIC::CIC_NUS_8303; break;
			case UINT64_C(0x000000aa764e39e1): cic = CIC::CIC_NUS_8401; break;
			case UINT64_C(0x000000abb0b739e1): cic = CIC::CIC_NUS_DDUS; break;
			case UINT64_C(0x00000081ce470326): // CIC-5101 IPL3
			case UINT64_C(0x000000827a47195a): // Kuru Kuru Fever 
			case UINT64_C(0x00000082551e4848): // Tower & Shaft
				cic = CIC::CIC_NUS_5101; break;
		}
	} while (cic == CIC::UNKNOWN);

	printf("Region: %s, CIC: CIC-NUS-%s\n", stringify(system_type), stringify(cic));

	if (!is_auto()) {
		printf("Auto-detect is OFF, not updating OSD settings\n");
		return true;
	}

	printf("Auto-detect is ON, updating OSD settings\n");

	user_io_status_set(SYS_TYPE_OPT, (uint32_t)system_type);
	user_io_status_set(CIC_TYPE_OPT, (uint32_t)cic);

	return is_known_cic;
}

// Creates a lower-case hex string out of the MD5 buffer
static void md5_to_hex(uint8_t* md5, char* out) {
	for (size_t i = 0; i < MD5_LENGTH; i++) {
		sprintf(out, "%02x", md5[i]);
		out += 2;
	}
}

static void calc_bootcode_checksums(uint64_t bootcode_sums[2], const uint8_t* buf) {
	size_t i;
	uint64_t sum = 0;

	// Calculate boot code checksum for bytes 0x40 - 0xc00 (Aleck64)
	for (i = 0x40 / sizeof(uint32_t); i < 0xc00 / sizeof(uint32_t); i++) {
		sum += ((uint32_t*)buf)[i];
	}

	bootcode_sums[1] = sum;

	// Calculate boot code checksum for bytes 0x40 - 0x1000
	for (; i < 0x1000 / sizeof(uint32_t); i++) {
		sum += ((uint32_t*)buf)[i];
	}

	bootcode_sums[0] = sum;
}

// Mount Save File. Return "true" if save file was created
static bool mount_save_file(const char *name, MemoryType type, const char *old_path) {
	char save_path[1024];
	create_path(SAVE_DIR, CoreName);
	sprintf(save_path, "%s/%s/", SAVE_DIR, CoreName);
	char* fname = save_path + strlen(save_path);

	const char* p = strrchr(name, '/');
	if (p) {
		strcat(fname, p + 1);
	}
	else {
		strcat(fname, name);
	}

	char ext[16];

	switch (type) {
		case MemoryType::EEPROM_512:
		case MemoryType::EEPROM_2k:
			strcpy(ext, ".eep");
			break;
		case MemoryType::SRAM_32k:
		case MemoryType::SRAM_96k:
			strcpy(ext, ".sra");
			break;
		case MemoryType::FLASH_128k:
			strcpy(ext, ".fla");
			break;
		case MemoryType::CPAK:
		case MemoryType::TPAK:
			sprintf(ext, "_%u%s", 
				(mounted_save_files + ((get_cart_save_type() == MemoryType::NONE) ? 1 : 0)), 
				((type == MemoryType::CPAK) ? ".cpk" : ".tpk"));
			break;
		default:
			strcpy(ext, ".sav");
			break;
	}

	char* e = strrchr(fname, '.');
	if (e) {
		strcpy(e, ext);
	}
	else {
		strcat(fname, ext);
	}

	auto save_file = new SaveFile(type);
	auto is_new = save_file->create_if_missing(save_path, old_path);
	save_file->mount(save_path);
	save_files[save_file->idx] = *save_file;
	return is_new;
}

static void get_old_save_path(char* save_path, const char *name) {
	constexpr auto ext = ".sav";

	create_path(SAVE_DIR, CoreName);
	sprintf(save_path, "%s/%s/", SAVE_DIR, CoreName);
	char* fname = save_path + strlen(save_path);

	const char* p = strrchr(name, '/');
	if (p) {
		strcat(fname, p + 1);
	}
	else {
		strcat(fname, name);
	}

	char* e = strrchr(fname, '.');
	if (e) {
		strcpy(e, ext);
	}
	else {
		strcat(fname, ext);
	}
}

void n64_load_savedata(uint64_t lba, int ack, uint64_t &buffer_lba, uint8_t* buffer, uint32_t buffer_size, uint32_t blksz, uint32_t sz) {
	int fio_size = user_io_get_width();
	buffer_lba = -1;
	int done = 0;
	int invalid = 0;

	unsigned char file_idx = 0;
	int64_t pos = lba * blksz;
	while (pos >= get_save_file_offset(file_idx + 1)) {
		file_idx++;
		if (file_idx >= mounted_save_files) {
			invalid = 1;
			buffer_lba = lba;
			break;
		}
	}

	if (!invalid) {
		fileTYPE* file = get_image(file_idx);
		pos -= get_save_file_offset(file_idx);

		if (file->size)
		{
			diskled_on();
			if (FileSeek(file, pos, SEEK_SET) && FileReadAdv(file, buffer, buffer_size)) {
				// printf("Loaded save data, %u bytes from %s (%lld)\n", sz, file->name, pos);
				done = 1;
				buffer_lba = lba;
			}
		}
	}

	//Even after error we have to provide the block to the core
	//Give an empty block.
	if (!done || invalid) {
		memset(buffer, 0, buffer_size);
	}
	else if ((save_files[file_idx].type == MemoryType::CPAK) || (save_files[file_idx].type == MemoryType::TPAK)) {
		normalizeData(buffer, sz, DataFormat::LITTLE_ENDIAN);
	}

	// data is now stored in buffer. send it to fpga
	EnableIO();
	spi_w(UIO_SECTOR_RD | ack);
	spi_block_write(buffer, fio_size, sz);
	DisableIO();
}

void n64_save_savedata(uint64_t lba, int ack, uint64_t& buffer_lba, uint8_t* buffer, uint32_t blksz, uint32_t sz) {
	menu_process_save();

	int fio_size = user_io_get_width();
	buffer_lba = -1;

	unsigned char file_idx = 0;
	int64_t pos = lba * blksz;
	int invalid = 0;

	while (pos >= get_save_file_offset(file_idx + 1)) {
		file_idx++;
		if (file_idx >= mounted_save_files) {
			invalid = 1;
			break;
		}
	}

	// Fetch sector data from FPGA ...
	EnableIO();
	spi_w(UIO_SECTOR_WR | ack);
	spi_block_read(buffer, fio_size, sz);
	DisableIO();

	if (invalid) {
		return;
	}

	auto file = get_image(file_idx);
	pos -= get_save_file_offset(file_idx);

	if (sz) {
		diskled_on();
		if (FileSeek(file, pos, SEEK_SET)) {
			if ((save_files[file_idx].type == MemoryType::CPAK) || (save_files[file_idx].type == MemoryType::TPAK)) {
				normalizeData(buffer, sz, DataFormat::LITTLE_ENDIAN);
			}
			if (!FileWriteAdv(file, buffer, sz)) {
				printf("Failed to write save data! (%u bytes to %s at %lld)\n", sz, file->name, pos);
			}
		}
	}
}

static void unmount_all_save_files()
{
	fileTYPE *file;
	for (auto i = 0U; i < (sizeof(save_files) / sizeof(SaveFile)); i++) {
		if ((file = get_image(i)) && file->opened()) {
			FileClose(file);
		}
		save_files[i] = {};
	}
	mounted_save_files = 0;
}

int n64_rom_tx(const char *name, unsigned char idx) {
	static uint8_t buf[4096];
	fileTYPE f;

	if (!FileOpen(&f, name, 1)) return 0;

	uint32_t data_size = f.size;
	auto data_left = data_size;

	printf("N64 file %s with %u bytes to send for index %04X\n", name, data_size, idx);

	// set index byte
	user_io_set_index(idx);

	// prepare transmission of new file
	user_io_set_download(1);

	const int use_progress = 1;
	if (use_progress) ProgressMessage(0, 0, 0, 0);

	if ((idx & 0x3f) == 2) {
		// Handle non-N64 files (GameBoy)
		while (data_left) {
			uint32_t chunk = (data_left > sizeof(buf)) ? sizeof(buf) : data_left;
			FileReadAdv(&f, buf, chunk);

			user_io_file_tx_data(buf, chunk);

			if (use_progress) ProgressMessage("Loading", f.name, data_size - data_left, data_size);
			data_left -= chunk;
		}

		printf("Done.\n");
		FileClose(&f);

		// Signal end of transmission
		user_io_set_download(0);
		ProgressMessage(0, 0, 0, 0);

		return 1;
	}

	// save state processing
	process_ss(name);

	unmount_all_save_files();

	/* 0 = Nothing detected
	   1 = System region and CIC detected
	   2 = Found some ROM info in DB (Save type etc.), but System region and/or CIC has not been determined
	   3 = Has detected everything, System type, CIC, Save type etc. */
	uint8_t rom_settings_detected = 0;
	DataFormat rom_format;
	uint8_t md5[MD5_LENGTH];
	char md5_hex[MD5_LENGTH * 2 + 1];
	uint64_t bootcode_sums[2] = { };
	uint8_t controller_settings[4];
	char cart_id[CARTID_LENGTH + 1] = { '\0', '\0', '\0', '\0', '0', '0' };
	bool is_first_chunk = true;
	char* internal_name = new char[21] { };

	MD5Context ctx;
	MD5Init(&ctx);

	while (data_left) {
		size_t chunk = (data_left > sizeof(buf)) ? sizeof(buf) : data_left;

		FileReadAdv(&f, buf, chunk);

		// perform sanity checks and detect ROM format
		if (is_first_chunk) {
			if (chunk < 4096) {
				printf("Failed to load ROM: must be at least 4096 bytes\n");
				Info("Invalid ROM!");

				// Signal end of transmission
				user_io_set_download(0);
				ProgressMessage(0, 0, 0, 0);
				return 0;
			}

			rom_format = detectRomFormat(buf);
		}

		// normalize data to big-endian format
		normalizeData(buf, chunk, rom_format);
		MD5Update(&ctx, buf, chunk);

		if (is_first_chunk) {
			/* Try to detect ROM settings based on header MD5 hash.
			   For calculating the MD5 hash of the header, we need to make a
			   copy of the context before calling MD5Final, otherwise the file
			   hash will be incorrect later on. */

			MD5Context ctx_header;
			memcpy(&ctx_header, &ctx, sizeof(struct MD5Context));
			MD5Final(md5, &ctx_header);
			md5_to_hex(md5, md5_hex);
			printf("Header MD5: %s\n", md5_hex);
			trim(internal_name, 20, (char *)(&buf[0x20]));

			rom_settings_detected = detect_rom_settings_in_dbs_with_md5(md5_hex);
			if (rom_settings_detected == 0) {
				printf("No ROM information found for header hash: %s\n", md5_hex);
			}

			memcpy(controller_settings, &buf[0x34], sizeof(controller_settings));
			calc_bootcode_checksums(bootcode_sums, buf);

			/* The first byte (starting at 0x3b) indicates the type of ROM
				 'N' = Cartridge
				 'D' = 64DD disk
				 'C' = Cartridge part of expandable game
				 'E' = 64DD expansion for cart
				 'Z' = Aleck64 cart
			   The 2nd and 3rd byte form a 2-letter ID for the game
			   The 4th byte indicates the region and language for the game
			   The 5th byte indicates the revision of the game */

			auto p_buf = (char *)&buf[0x3b];
			for (auto i = 0; i < 4; i++, p_buf++) {
				if (isalnum(*p_buf)) {
					cart_id[i] = *p_buf;
				}
				else {
					cart_id[i] = '?';
				}
			}

			if (strncmp(cart_id, "????", 4) != 0) {
				sprintf(cart_id + 4, "%02X", buf[0x3f]);
				printf("Cartridge ID: %s\n", cart_id);
			}
			else {
				memset(cart_id, '\0', CARTID_LENGTH);
			}
		}

		user_io_file_tx_data(buf, chunk);

		if (use_progress) ProgressMessage("Loading", f.name, data_size - data_left, data_size);
		data_left -= chunk;
		is_first_chunk = false;
	}

	MD5Final(md5, &ctx);
	md5_to_hex(md5, md5_hex);
	printf("File MD5: %s\n", md5_hex); 

	// Try to detect ROM settings from full file MD5 if they're are not detected yet
	if (rom_settings_detected == 0) {
		rom_settings_detected = detect_rom_settings_in_dbs_with_md5(md5_hex);
	}

	// Try to detect ROM settings by cart ID if they're are not detected yet
	if (rom_settings_detected == 0) {
		printf("No ROM information found for file hash: %s\n", md5_hex);
		rom_settings_detected = detect_rom_settings_in_dbs_with_cartid(cart_id);
		if (rom_settings_detected == 0) {
			if (detect_homebrew_header(controller_settings, cart_id)) {
				rom_settings_detected = 2;
			}
			else {
				printf("No ROM information found for Cart ID: %s\n", cart_id);
				if (is_auto()) {
					// Defaulting misc. System Settings
					if (old_ar != AspectRatio::UNKNOWN) {
						user_io_status_set(AR_TYPE_OPT, (uint32_t)old_ar); // Resetting Aspect Ratio to Original
						old_ar = AspectRatio::UNKNOWN;
					}
					user_io_status_set(NO_EPAK_OPT, 0); // Enable Expansion Pak
					user_io_status_set(CPAK_OPT, 0); // Disable Controller Pak
					user_io_status_set(RPAK_OPT, 0); // Disable Rumble Pak
					user_io_status_set(TPAK_OPT, 0); // Disable Transfer Pak
					user_io_status_set(RTC_OPT, 0); // Disable RTC
					set_cart_save_type(MemoryType::NONE);
				}
			}
		}

		if ((rom_settings_detected == 0 || rom_settings_detected == 2) &&
			detect_rom_settings_from_first_chunk(cart_id[3], bootcode_sums)) {
			// Try detect (partial) ROM settings by analyzing the ROM itself. (System region and CIC)
			rom_settings_detected |= 1;
		}
	}
	else if (rom_settings_detected == 2 &&
		detect_rom_settings_from_first_chunk(cart_id[3], bootcode_sums)) {
		// Complement info found in DB with System region and CIC
		rom_settings_detected = 3;
	}

	printf("Done.\n");
	FileClose(&f);

	auto save_type = get_cart_save_type();
	char old_save_path[1024];
	get_old_save_path(old_save_path, name);
	bool do_reset = false;

	if (save_type != MemoryType::NONE) {
		do_reset |= mount_save_file(name, save_type, old_save_path);
	}

	auto use_cpak = (bool)user_io_status_get(CPAK_OPT);
	auto use_tpak = (bool)user_io_status_get(TPAK_OPT);

	// First controller can be either tpak or cpak. Tpak is prioritized.
	if (use_tpak || use_cpak) {
		do_reset |= mount_save_file(name,
			(use_tpak ? MemoryType::TPAK : MemoryType::CPAK), 
			old_save_path);
	}

	if (use_cpak) {
		do_reset |= mount_save_file(name, MemoryType::CPAK, old_save_path);
		do_reset |= mount_save_file(name, MemoryType::CPAK, old_save_path);
		do_reset |= mount_save_file(name, MemoryType::CPAK, old_save_path);
	}

	// Signal end of transmission
	user_io_set_download(0);

	ProgressMessage(0, 0, 0, 0);

	// reset if new save files were 
	if (do_reset) {
		user_io_status_set("[0]", 1);
		usleep(100000);
		user_io_status_set("[0]", 0);
	}

	if (!is_auto()) {
		return 1;
	}

	auto system_type = SystemType::UNKNOWN;
	auto cic = CIC::UNKNOWN;

	char info[256];
	size_t len = sprintf(info, "Auto-detect:");
	if (*cart_id && ((cart_id[1] != 'E') || (cart_id[2] != 'D'))) len += sprintf(info + len, "\n[%.4s] v.%u.%u", cart_id, hex_to_dec(cart_id[4]) + 1, hex_to_dec(cart_id[5]));
	if (*internal_name) len += sprintf(info + len, "\n\"%s\"", internal_name);
	if ((rom_settings_detected & 1) == 0) {
		len += sprintf(info + len, "\nUnknown Region/CIC");
	}
	else {
		system_type = (SystemType)user_io_status_get(SYS_TYPE_OPT);
		cic = (CIC)user_io_status_get(CIC_TYPE_OPT);
		len += sprintf(info + len, "\nRegion: %s (%s)", stringify(system_type), stringify(cic));
	}

	if ((rom_settings_detected & 2) == 0) {
		sprintf(info + len, "\nROM missing from database.\nYou might not be able to save.");

		Info(info, 4000);
	}
	else {
		auto no_epak = (bool)user_io_status_get(NO_EPAK_OPT);
		auto tpak = (bool)user_io_status_get(TPAK_OPT);
		auto cpak = (bool)user_io_status_get(CPAK_OPT);
		auto rpak = (bool)user_io_status_get(RPAK_OPT);
		auto rtc = (bool)user_io_status_get(RTC_OPT);

		if (save_type != MemoryType::NONE) len += sprintf(info + len, "\nSave Type: %s", stringify(save_type));
		if (tpak) len += sprintf(info + len, "\nTransfer Pak \x96");
		if (cpak) len += sprintf(info + len, "\nController Pak \x96");
		if (rpak) len += sprintf(info + len, "\nRumble Pak \x96");
		if (rtc) len += sprintf(info + len, "\nRTC \x96");
		if (no_epak) sprintf(info + len, "\nDisable Exp. Pak \x96");

		Info(info, 6000);
	}

	return 1;
}
