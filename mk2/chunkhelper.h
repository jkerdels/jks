#ifndef JK_CHUNKHELPER_H
#define JK_CHUNKHELPER_H

namespace jk {

uint32_t chunk_from_str(const char *id)
{
	uint32_t result = 0;

	if (id == nullptr) return 0;

	int i = 0;
	uint32_t cc = 0;
	while((cc = (uint32_t)id[i]) && (i < 4)) {
		result |= cc << (i++ * 8);
	}
	return result;
}




}

#endif
