#include <assert.h>
#include <string.h>

#include "BlockData.hpp"
#include "ColorSpace.hpp"
#include "CpuArch.hpp"
#include "Debug.hpp"
#include "Dither.hpp"
#include "MipMap.hpp"
#include "mmap.hpp"
#include "ProcessAlpha.hpp"
#include "ProcessRGB.hpp"
#include "ProcessRGB_AVX2.hpp"
#include "Tables.hpp"
#include "TaskDispatch.hpp"

#include "squish/squish.h"

BlockData::BlockData( const char* fn )
{
	m_etc1.file = fopen(fn, "rb");
    assert( m_etc1.file );
    fseek( m_etc1.file, 0, SEEK_END );
    m_maplen = ftell( m_etc1.file );
    fseek( m_etc1.file, 0, SEEK_SET );
    m_etc1.data = (uint8*)mmap( nullptr, m_maplen, PROT_READ, MAP_SHARED, fileno( m_etc1.file ), 0 );

    auto data32 = (uint32*)m_etc1.data;
    if( *data32 == 0x03525650 )
    {
        m_size.y = *(data32+6);
        m_size.x = *(data32+7);
        m_etc1.offset = 52 + *(data32+12);
    }
    else if( *data32 == 0x58544BAB )
    {
        m_size.x = *(data32+9);
        m_size.y = *(data32+10);
        m_etc1.offset = 17 + *(data32+15);
    }
    else
    {
        assert( false );
    }
}

typedef struct {
	uint32 tag;
	uint32 flags;
	uint32 pixel_formats[2];
	uint32 colour_space;
	uint32 channel_type;
	uint32 height;
	uint32 width;
	uint32 depth;
	uint32 num_surfs;
	uint32 num_faces;
	uint32 mipmaps;
	uint32 reversed;
} PVRHeader;

typedef struct {
	uint8 tag[6];
	uint16 format;
	uint16 tex_width;
	uint16 tex_height;
	uint16 orig_width;
	uint16 orig_height;
} PKMHeader;

typedef struct {
	char tag[4];
	uint32 hsize;
	uint32 flags;
	uint32 height;
	uint32 width;
	uint32 pitch;
	uint32 depth;
	uint32 mipmaps;
	uint32 dummy[11];

	uint32 format_size;
	uint32 format_flags;
	uint32 format_fourcc;
	uint32 format_rgb_bits;
	uint32 format_red_mask;
	uint32 format_green_mask;
	uint32 format_blue_mask;
	uint32 format_alpha_mask;

	uint32 caps_1;
	uint32 caps_2;
	uint32 caps_ddsx;

	uint32 skip1;
	uint32 skip2;

} DDSHeader;

typedef enum {
	FormatPvr,
	FormatPkm,
	FormatDds,
} eFormat;

static uint8* OpenForWriting( const char* fn_, size_t len, const v2i& size, FILE** f, int levels, bool atlas, eFormat fmt)
{
	std::string fn = fn_;
	switch(fmt) {
	case FormatPvr:
		fn += ".pvr";
		break;
	case FormatPkm:
		fn += ".pkm";
		break;
	case FormatDds:
		fn += ".dds";
		break;
	}

    *f = fopen( fn.c_str(), "wb+" );
    assert( *f );
    fseek( *f, len - 1, SEEK_SET );
    const char zero = 0;
    fwrite( &zero, 1, 1, *f );
    fseek( *f, 0, SEEK_SET );

    auto ret = (uint8*)mmap( nullptr, len, PROT_WRITE, MAP_SHARED, fileno( *f ), 0 );
    auto dst = (uint32*)ret;

	switch(fmt) {
	case FormatPkm:
	{
		PKMHeader *h = (PKMHeader *) ret;
		memcpy(h->tag, "PKM 10", sizeof(h->tag));
		h->format = levels - 1;
		h->orig_height = _byteswap_ushort(atlas ? size.y * 2: size.y);
		h->orig_width = _byteswap_ushort(size.x);
		h->tex_height = h->orig_height;
		h->tex_width = h->orig_width;
	}
	break;
	case FormatPvr:
	{
		PVRHeader *h = (PVRHeader *) ret;
		h->tag = 0x03525650;
		h->flags = 0;
		h->pixel_formats[0] = 6; // pixelformat[0], value 22 is needed for etc2
		h->pixel_formats[1] = 0;
		h->colour_space = 0;
		h->channel_type = 0;
		h->height = atlas ? size.y * 2 : size.y;
		h->width = size.x;
		h->depth = 1;
		h->num_surfs = 1;
		h->num_faces = 1;
		h->mipmaps = levels;
		h->reversed = 0;
	}
	break;
	case FormatDds:
	{
		DDSHeader *h = (DDSHeader *) ret;
		strncpy(h->tag, "DDS ", 4);
		h->hsize = 124;
		h->flags = 0x000A1007;
		h->height = atlas ? size.y * 2 : size.y;
		h->width = size.x;
		h->pitch = h->width * h->height / 2;
		h->depth = 0;
		h->mipmaps = levels;
		memset(h->dummy, 0, sizeof(h->dummy));
		h->format_size = 0x20;
		h->format_flags = 5;
		h->format_fourcc = 0x31545844; // DXT1
		h->format_rgb_bits = 0;
		h->format_red_mask = 0;
		h->format_green_mask = 0;
		h->format_blue_mask = 0;
		h->format_alpha_mask = 0;

		h->caps_1 = 0x00401008;
		h->caps_2 = 0;
		h->caps_ddsx = 0;
		h->skip1 = 0;
		h->skip2 = 0;
	}
	break;
	}
    return ret;
}

static int AdjustSizeForMipmaps( const v2i& size, int levels )
{
    int len = 0;
    v2i current = size;
    for( int i=1; i<levels; i++ )
    {
        assert( current.x != 1 || current.y != 1 );
        current.x = std::max( 1, current.x / 2 );
        current.y = std::max( 1, current.y / 2 );
        len += std::max( 4, current.x ) * std::max( 4, current.y ) / 2;
    }
    assert( current.x == 1 && current.y == 1 );
    return len;
}

BlockData::BlockData( const char* fn, const v2i& size, bool mipmap, bool atlas, bool etc_pkm, bool dds )
    : m_size( size )
{
	size_t hsize = (etc_pkm ? sizeof(PKMHeader) : sizeof(PVRHeader));
    m_etc1.offset = hsize;
    m_maplen = m_size.x*m_size.y/2;
    assert( m_size.x%4 == 0 && m_size.y%4 == 0 );

    uint32 cnt = m_size.x * m_size.y / 16;
    DBGPRINT( cnt << " blocks" );

    int levels = 1;

    if( mipmap )
    {
        levels = NumberOfMipLevels( size );
        DBGPRINT( "Number of mipmaps: " << levels );
        m_maplen += AdjustSizeForMipmaps( size, levels );
    }
	if(atlas)
		m_maplen *= 2;

    m_etc1.data = OpenForWriting( fn, hsize + m_maplen, m_size, &m_etc1.file, levels, atlas, (etc_pkm ? FormatPkm : FormatPvr) );
	if(atlas)
		m_etc1.atlas = m_etc1.data + (hsize + m_maplen / 2);

	if(dds) {
		m_dds.offset = sizeof(DDSHeader);
	    m_dds.data = OpenForWriting( fn, sizeof(DDSHeader) + m_maplen, m_size, &m_etc1.file, levels, atlas, FormatDds );
		if(atlas)
			m_dds.atlas = m_dds.data + (sizeof(DDSHeader) + m_maplen / 2);
	}
	m_maplen += hsize;
}

BlockData::BlockData( const v2i& size, bool mipmap )
    : m_size( size )
    , m_maplen( 52 + m_size.x*m_size.y/2 )
{
    m_etc1.offset = sizeof(PVRHeader);
    assert( m_size.x%4 == 0 && m_size.y%4 == 0 );
    if( mipmap )
    {
        const int levels = NumberOfMipLevels( size );
        m_maplen += AdjustSizeForMipmaps( size, levels );
    }
    m_etc1.data = new uint8[m_maplen];
}

BlockData::~BlockData()
{
    if( m_etc1.file )
    {
        munmap( m_etc1.data, m_maplen );
        fclose( m_etc1.file );
    }
    else
    {
        delete[] m_etc1.data;
    }
}

static uint64 _f_rgb( uint8* ptr )
{
    return ProcessRGB( ptr );
}

static uint64 _f_rgb_avx2( uint8* ptr )
{
    return ProcessRGB_AVX2( ptr );
}

static uint64 _f_rgb_dither( uint8* ptr )
{
    Dither( ptr );
    return ProcessRGB( ptr );
}

static uint64 _f_rgb_dither_avx2( uint8* ptr )
{
    Dither( ptr );
    return ProcessRGB_AVX2( ptr );
}

static uint64 _f_rgb_etc2( uint8* ptr )
{
    return ProcessRGB_ETC2( ptr );
}

static uint64 _f_rgb_etc2_avx2( uint8* ptr )
{
    return ProcessRGB_ETC2_AVX2( ptr );
}

static uint64 _f_rgb_etc2_dither( uint8* ptr )
{
    Dither( ptr );
    return ProcessRGB_ETC2( ptr );
}

static uint64 _f_rgb_etc2_dither_avx2( uint8* ptr )
{
    Dither( ptr );
    return ProcessRGB_ETC2_AVX2( ptr );
}

#pragma pack(push,1)
typedef struct {
	uint8 r, g, b, a;
} Pixel;
#pragma pack(pop)

void BlockData::Process( const uint32* src, uint32 blocks, size_t offset, size_t width, Channels type, bool dither, bool etc2 )
{
    uint32 buf[4*4];
    int w = 0;

	uint64 *dst, *dst_dds;
	if(type == Channels::Alpha && m_etc1.atlas) {
		dst = ((uint64*)( m_etc1.atlas )) + offset;
		if(m_dds.atlas)
			dst_dds = ((uint64*)( m_dds.atlas )) + offset;
	}
	else {
		dst = ((uint64*)( m_etc1.data + m_etc1.offset )) + offset;
		if(m_dds.data)
			dst_dds = ((uint64*)( m_dds.data + m_dds.offset )) + offset;
	}

    uint64 (*func)(uint8*);

    if( type == Channels::Alpha )
    {
#ifdef __SSE4_1__
        if( can_use_intel_core_4th_gen_features() )
        {
            if( etc2 )
            {
                func = _f_rgb_etc2_avx2;
            }
            else
            {
                func = _f_rgb_avx2;
            }
        }
        else
#endif
        {
            if( etc2 )
            {
                func = _f_rgb_etc2;
            }
            else
            {
                func = _f_rgb;
            }
        }

        do
        {
			const uint32 *src_dds = src;

            auto ptr = buf;
            for( int x=0; x<4; x++ )
            {
                uint a = *src >> 24;
                *ptr++ = a | ( a << 8 ) | ( a << 16 ) | 0xFF000000;
                src += width;
                a = *src >> 24;
                *ptr++ = a | ( a << 8 ) | ( a << 16 ) | 0xFF000000;
                src += width;
                a = *src >> 24;
                *ptr++ = a | ( a << 8 ) | ( a << 16 ) | 0xFF000000;
                src += width;
                a = *src >> 24;
                *ptr++ = a | ( a << 8 ) | ( a << 16 ) | 0xFF000000;
                src -= width * 3 - 1;
            }
            if( ++w == width/4 )
            {
                src += width * 3;
                w = 0;
            }

            *dst++ = func( (uint8*)buf );

			if(dst_dds) {

				auto ptr = buf;
				for(int y = 0; y < 4; y ++) {

					for(int x = 0; x < 4; x++) {

						uint32 a = *src_dds++ >> 24;
						*ptr++ = a | (a << 8) | (a << 16) | 0xFF000000;
					}
					src_dds += width - 4;
				}
				squish::Compress((squish::u8*)buf, dst_dds++, squish::kDxt1);
			}
        }
        while( --blocks );
    }
    else
    {
#ifdef __SSE4_1__
        if( can_use_intel_core_4th_gen_features() )
        {
            if( etc2 )
            {
                if( dither )
                {
                    func = _f_rgb_etc2_dither_avx2;
                }
                else
                {
                    func = _f_rgb_etc2_avx2;
                }
            }
            else
            {
                if( dither )
                {
                    func = _f_rgb_dither_avx2;
                }
                else
                {
                    func = _f_rgb_avx2;
                }
            }
        }
        else
#endif
        {
            if( etc2 )
            {
                if( dither )
                {
                    func = _f_rgb_etc2_dither;
                }
                else
                {
                    func = _f_rgb_etc2;
                }
            }
            else
            {
                if( dither )
                {
                    func = _f_rgb_dither;
                }
                else
                {
                    func = _f_rgb;
                }
            }
        }

        do
        {
			const uint32 *src_dds = src;

            auto ptr = buf;
            for( int x=0; x<4; x++ )
            {
                *ptr++ = *src;
                src += width;
                *ptr++ = *src;
                src += width;
                *ptr++ = *src;
                src += width;
                *ptr++ = *src;
                src -= width * 3 - 1;
            }
            if( ++w == width/4 )
            {
                src += width * 3;
                w = 0;
            }
            *dst++ = func( (uint8*)buf );

			if(dst_dds) {

				auto ptr = buf;
				for(int y = 0; y < 4; y ++) {

					for(int x = 0; x < 4; x++) {

						Pixel& s = *(Pixel *) src_dds++;
						Pixel p;
						p.r = s.b;
						p.g = s.g;
						p.b = s.r;
						p.a = 255;
						*ptr++ = *(uint32 *)&p;
					}
					src_dds += width - 4;
				}
				squish::Compress((squish::u8*)buf, dst_dds++, squish::kDxt1);
			}
        }
        while( --blocks );
    }
}

namespace
{
struct BlockColor
{
    uint32 r1, g1, b1;
    uint32 r2, g2, b2;
};

enum class Etc2Mode
{
    none,
    t,
    h,
    planar
};

Etc2Mode DecodeBlockColor( uint64 d, BlockColor& c )
{
    if( d & 0x2 )
    {
        int32 dr, dg, db;

        c.r1 = ( d & 0xF8000000 ) >> 27;
        c.g1 = ( d & 0x00F80000 ) >> 19;
        c.b1 = ( d & 0x0000F800 ) >> 11;

        dr = ( d & 0x07000000 ) >> 24;
        dg = ( d & 0x00070000 ) >> 16;
        db = ( d & 0x00000700 ) >> 8;

        if( dr & 0x4 )
        {
            dr |= 0xFFFFFFF8;
        }
        if( dg & 0x4 )
        {
            dg |= 0xFFFFFFF8;
        }
        if( db & 0x4 )
        {
            db |= 0xFFFFFFF8;
        }

        int32 r = static_cast<int32_t>(c.r1) + dr;
        int32 g = static_cast<int32_t>(c.g1) + dg;
        int32 b = static_cast<int32_t>(c.b1) + db;

        if ((r < 0) || (r > 31))
        {
            return Etc2Mode::t;
        }

        if ((g < 0) || (g > 31))
        {
            return Etc2Mode::h;
        }

        if ((b < 0) || (b > 31))
        {
            return Etc2Mode::planar;
        }

        c.r2 = c.r1 + dr;
        c.g2 = c.g1 + dg;
        c.b2 = c.b1 + db;

        c.r1 = ( c.r1 << 3 ) | ( c.r1 >> 2 );
        c.g1 = ( c.g1 << 3 ) | ( c.g1 >> 2 );
        c.b1 = ( c.b1 << 3 ) | ( c.b1 >> 2 );
        c.r2 = ( c.r2 << 3 ) | ( c.r2 >> 2 );
        c.g2 = ( c.g2 << 3 ) | ( c.g2 >> 2 );
        c.b2 = ( c.b2 << 3 ) | ( c.b2 >> 2 );
    }
    else
    {
        c.r1 = ( ( d & 0xF0000000 ) >> 24 ) | ( ( d & 0xF0000000 ) >> 28 );
        c.r2 = ( ( d & 0x0F000000 ) >> 20 ) | ( ( d & 0x0F000000 ) >> 24 );
        c.g1 = ( ( d & 0x00F00000 ) >> 16 ) | ( ( d & 0x00F00000 ) >> 20 );
        c.g2 = ( ( d & 0x000F0000 ) >> 12 ) | ( ( d & 0x000F0000 ) >> 16 );
        c.b1 = ( ( d & 0x0000F000 ) >> 8  ) | ( ( d & 0x0000F000 ) >> 12 );
        c.b2 = ( ( d & 0x00000F00 ) >> 4  ) | ( ( d & 0x00000F00 ) >> 8  );
    }
    return Etc2Mode::none;
}

inline int32 expand6(uint32 value)
{
    return (value << 2) | (value >> 4);
}

inline int32 expand7(uint32 value)
{
    return (value << 1) | (value >> 6);
}

void DecodePlanar(uint64 block, uint32* l[4])
{
    const auto bv = expand6((block >> ( 0 + 32)) & 0x3F);
    const auto gv = expand7((block >> ( 6 + 32)) & 0x7F);
    const auto rv = expand6((block >> (13 + 32)) & 0x3F);

    const auto bh = expand6((block >> (19 + 32)) & 0x3F);
    const auto gh = expand7((block >> (25 + 32)) & 0x7F);

    const auto rh0 = (block >> (32 - 32)) & 0x01;
    const auto rh1 = ((block >> (34 - 32)) & 0x1F) << 1;
    const auto rh = expand6(rh0 | rh1);

    const auto bo0 = (block >> (39 - 32)) & 0x07;
    const auto bo1 = ((block >> (43 - 32)) & 0x3) << 3;
    const auto bo2 = ((block >> (48 - 32)) & 0x1) << 5;
    const auto bo = expand6(bo0 | bo1 | bo2);
    const auto go0 = (block >> (49 - 32)) & 0x3F;
    const auto go1 = ((block >> (56 - 32)) & 0x01) << 6;
    const auto go = expand7(go0 | go1);
    const auto ro = expand6((block >> (57 - 32)) & 0x3F);

    for (auto j = 0; j < 4; j++)
    {
        for (auto i = 0; i < 4; i++)
        {
            uint32 r = clampu8((i * (rh - ro) + j * (rv - ro) + 4 * ro + 2) >> 2);
            uint32 g = clampu8((i * (gh - go) + j * (gv - go) + 4 * go + 2) >> 2);
            uint32 b = clampu8((i * (bh - bo) + j * (bv - bo) + 4 * bo + 2) >> 2);

            *l[j]++ = r | ( g << 8 ) | ( b << 16 ) | 0xFF000000;
        }
    }
}

}

BitmapPtr BlockData::Decode()
{
	v2i size = m_size;
	if(m_etc1.atlas)
		size.y *= 2;
    auto ret = std::make_shared<Bitmap>( size );

    uint32* l[4];
    l[0] = ret->Data();
    l[1] = l[0] + size.x;
    l[2] = l[1] + size.x;
    l[3] = l[2] + size.x;

    const uint64* src = (const uint64*)( m_etc1.data + m_etc1.offset );

    for( int y=0; y<size.y/4; y++ )
    {
        for( int x=0; x<size.x/4; x++ )
        {
            uint64 d = *src++;

            d = ( ( d & 0xFF000000FF000000 ) >> 24 ) |
                ( ( d & 0x000000FF000000FF ) << 24 ) |
                ( ( d & 0x00FF000000FF0000 ) >> 8 ) |
                ( ( d & 0x0000FF000000FF00 ) << 8 );

            BlockColor c;
            const auto mode = DecodeBlockColor( d, c );

            if (mode == Etc2Mode::planar)
            {
                DecodePlanar(d, l);
                continue;
            }

            uint tcw[2];
            tcw[0] = ( d & 0xE0 ) >> 5;
            tcw[1] = ( d & 0x1C ) >> 2;

            uint ra, ga, ba;
            uint rb, gb, bb;
            uint rc, gc, bc;
            uint rd, gd, bd;

            if( d & 0x1 )
            {
                int o = 0;
                for( int i=0; i<4; i++ )
                {
                    ra = clampu8( c.r1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ga = clampu8( c.g1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ba = clampu8( c.b1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );

                    rb = clampu8( c.r1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    gb = clampu8( c.g1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    bb = clampu8( c.b1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );

                    rc = clampu8( c.r2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    gc = clampu8( c.g2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    bc = clampu8( c.b2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );

                    rd = clampu8( c.r2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    gd = clampu8( c.g2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    bd = clampu8( c.b2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );

                    *l[0]++ = ra | ( ga << 8 ) | ( ba << 16 ) | 0xFF000000;
                    *l[1]++ = rb | ( gb << 8 ) | ( bb << 16 ) | 0xFF000000;
                    *l[2]++ = rc | ( gc << 8 ) | ( bc << 16 ) | 0xFF000000;
                    *l[3]++ = rd | ( gd << 8 ) | ( bd << 16 ) | 0xFF000000;

                    o += 4;
                }
            }
            else
            {
                int o = 0;
                for( int i=0; i<2; i++ )
                {
                    ra = clampu8( c.r1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ga = clampu8( c.g1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ba = clampu8( c.b1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );

                    rb = clampu8( c.r1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    gb = clampu8( c.g1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    bb = clampu8( c.b1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );

                    rc = clampu8( c.r1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    gc = clampu8( c.g1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    bc = clampu8( c.b1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );

                    rd = clampu8( c.r1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    gd = clampu8( c.g1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    bd = clampu8( c.b1 + g_table[tcw[0]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );

                    *l[0]++ = ra | ( ga << 8 ) | ( ba << 16 ) | 0xFF000000;
                    *l[1]++ = rb | ( gb << 8 ) | ( bb << 16 ) | 0xFF000000;
                    *l[2]++ = rc | ( gc << 8 ) | ( bc << 16 ) | 0xFF000000;
                    *l[3]++ = rd | ( gd << 8 ) | ( bd << 16 ) | 0xFF000000;

                    o += 4;
                }
                for( int i=0; i<2; i++ )
                {
                    ra = clampu8( c.r2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ga = clampu8( c.g2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ba = clampu8( c.b2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );

                    rb = clampu8( c.r2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    gb = clampu8( c.g2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    bb = clampu8( c.b2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );

                    rc = clampu8( c.r2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    gc = clampu8( c.g2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    bc = clampu8( c.b2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );

                    rd = clampu8( c.r2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    gd = clampu8( c.g2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    bd = clampu8( c.b2 + g_table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );

                    *l[0]++ = ra | ( ga << 8 ) | ( ba << 16 ) | 0xFF000000;
                    *l[1]++ = rb | ( gb << 8 ) | ( bb << 16 ) | 0xFF000000;
                    *l[2]++ = rc | ( gc << 8 ) | ( bc << 16 ) | 0xFF000000;
                    *l[3]++ = rd | ( gd << 8 ) | ( bd << 16 ) | 0xFF000000;

                    o += 4;
                }
            }
        }

        l[0] += size.x * 3;
        l[1] += size.x * 3;
        l[2] += size.x * 3;
        l[3] += size.x * 3;
    }

    return ret;
}

// Block type:
//  red - 2x4, green - 4x2, blue - planar
//  dark - 444, bright - 555 + 333
void BlockData::Dissect()
{
    auto size = m_size / 4;
    const uint64* data = (const uint64*)( m_etc1.data + m_etc1.offset );

    auto src = data;

    auto bmp = std::make_shared<Bitmap>( size );
    auto dst = bmp->Data();

    auto bmp2 = std::make_shared<Bitmap>( m_size );
    uint32* l[4];
    l[0] = bmp2->Data();
    l[1] = l[0] + m_size.x;
    l[2] = l[1] + m_size.x;
    l[3] = l[2] + m_size.x;

    auto bmp3 = std::make_shared<Bitmap>( size );
    auto dst3 = bmp3->Data();

    for( int y=0; y<size.y; y++ )
    {
        for( int x=0; x<size.x; x++ )
        {
            uint64 d = *src++;

            d = ( ( d & 0xFF000000FF000000 ) >> 24 ) |
                ( ( d & 0x000000FF000000FF ) << 24 ) |
                ( ( d & 0x00FF000000FF0000 ) >> 8 ) |
                ( ( d & 0x0000FF000000FF00 ) << 8 );

            BlockColor c;
            const auto mode = DecodeBlockColor( d, c );

            switch( mode )
            {
            case Etc2Mode::none:
                switch( d & 0x3 )
                {
                case 0:
                    *dst++ = 0xFF000088;
                    break;
                case 1:
                    *dst++ = 0xFF008800;
                    break;
                case 2:
                    *dst++ = 0xFF0000FF;
                    break;
                case 3:
                    *dst++ = 0xFF00FF00;
                    break;
                default:
                    assert( false );
                    break;
                }
                break;
            case Etc2Mode::planar:
                *dst++ = 0xFFFF0000;
                break;
            default:
                assert( false );
                break;
            }

            uint tcw[2];
            tcw[0] = ( d & 0xE0 );
            tcw[1] = ( d & 0x1C ) << 3;

            *dst3++ = 0xFF000000 | ( tcw[0] << 8 ) | ( tcw[1] );

            if( d & 0x1 )
            {
                for( int i=0; i<4; i++ )
                {
                    *l[0]++ = 0xFF000000 | ( c.b1 << 16 ) | ( c.g1 << 8 ) | c.r1;
                    *l[1]++ = 0xFF000000 | ( c.b1 << 16 ) | ( c.g1 << 8 ) | c.r1;
                    *l[2]++ = 0xFF000000 | ( c.b2 << 16 ) | ( c.g2 << 8 ) | c.r2;
                    *l[3]++ = 0xFF000000 | ( c.b2 << 16 ) | ( c.g2 << 8 ) | c.r2;
                }
            }
            else
            {
                for( int i=0; i<2; i++ )
                {
                    *l[0]++ = 0xFF000000 | ( c.b1 << 16 ) | ( c.g1 << 8 ) | c.r1;
                    *l[1]++ = 0xFF000000 | ( c.b1 << 16 ) | ( c.g1 << 8 ) | c.r1;
                    *l[2]++ = 0xFF000000 | ( c.b1 << 16 ) | ( c.g1 << 8 ) | c.r1;
                    *l[3]++ = 0xFF000000 | ( c.b1 << 16 ) | ( c.g1 << 8 ) | c.r1;
                }
                for( int i=0; i<2; i++ )
                {
                    *l[0]++ = 0xFF000000 | ( c.b2 << 16 ) | ( c.g2 << 8 ) | c.r2;
                    *l[1]++ = 0xFF000000 | ( c.b2 << 16 ) | ( c.g2 << 8 ) | c.r2;
                    *l[2]++ = 0xFF000000 | ( c.b2 << 16 ) | ( c.g2 << 8 ) | c.r2;
                    *l[3]++ = 0xFF000000 | ( c.b2 << 16 ) | ( c.g2 << 8 ) | c.r2;
                }
            }
        }
        l[0] += m_size.x * 3;
        l[1] += m_size.x * 3;
        l[2] += m_size.x * 3;
        l[3] += m_size.x * 3;
    }

    bmp->Write( "out_block_type.png" );
    bmp2->Write( "out_block_color.png" );
    bmp3->Write( "out_block_selectors.png" );
}
