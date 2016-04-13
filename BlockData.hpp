#ifndef __BLOCKDATA_HPP__
#define __BLOCKDATA_HPP__

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <vector>

#include "Bitmap.hpp"
#include "Types.hpp"
#include "Vector.hpp"

class BlockData
{
public:
    BlockData( const char* fn );
    BlockData( const char* fn, const v2i& size, bool mipmap, bool atlas, bool etc_pkm );
    BlockData( const v2i& size, bool mipmap );
    ~BlockData();

    BitmapPtr Decode();
    void Dissect();

    void Process( const uint32* src, uint32 blocks, size_t offset, size_t width, Channels type, bool dither, bool etc2 );

private:
    uint8* m_data;
	uint8* m_atlas;
    v2i m_size;
    size_t m_dataOffset;
    FILE* m_file;
    size_t m_maplen;
};

typedef std::shared_ptr<BlockData> BlockDataPtr;

#endif
