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
