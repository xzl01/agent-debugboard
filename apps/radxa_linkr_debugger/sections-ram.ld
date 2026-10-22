SECTION_PROLOGUE(.bss.pre_capture, (NOLOAD),)
{
	__bss_pre_capture_start = .;
	KEEP(*(.bss.pre_capture.sigrok_runtime))
	KEEP(*(.bss.pre_capture.ws_clients))
	__bss_pre_capture_end = .;
} GROUP_NOLOAD_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)

ASSERT(SIZEOF(.bss.pre_capture) == 0xDA40, "pre-capture size")
ASSERT(__data_region_end <= 0x20010000, "pre-capture start")
ASSERT(__bss_start == 0x20010000, "generic BSS start")
ASSERT((__bss_start & 0x7fff) == 0, "generic BSS alignment")

/* Hard image budget: the RAM image area is 532480 bytes. Keep at least
 * 32768 bytes free, so the image must not exceed 499712 bytes (0x7A000).
 */
ASSERT(_image_ram_size <= 0x7A000,
       "RAM image exceeds 499712 B: _image_ram_size must stay <= 0x7A000, reserving 32768 B of 532480 B")
