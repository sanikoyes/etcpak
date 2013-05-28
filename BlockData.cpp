#include <assert.h>
#include <future>
#include <vector>

#include "BlockData.hpp"
#include "ColorSpace.hpp"
#include "Debug.hpp"

static const int32 table[][4] = {
    {  2,  8,   -2,   -8 },
    {  5, 17,   -5,  -17 },
    {  9, 29,   -9,  -29 },
    { 13, 42,  -13,  -42 },
    { 18, 60,  -18,  -60 },
    { 24, 80,  -24,  -80 },
    { 33, 106, -33, -106 },
    { 47, 183, -47, -183 } };

static v3b Average( const uint8* data )
{
    uint32 r = 0, g = 0, b = 0;
    for( int i=0; i<8; i++ )
    {
        b += *data++;
        g += *data++;
        r += *data++;
    }
    return v3b( r / 8, g / 8, b / 8 );
}

static Color::Lab Average( const Color::Lab* data )
{
    Color::Lab ret;
    for( int i=0; i<8; i++ )
    {
        ret.L += data->L;
        ret.a += data->a;
        ret.b += data->b;
        data++;
    }
    ret.L /= 8;
    ret.a /= 8;
    ret.b /= 8;
    return ret;
}

static float CalcError( Color::Lab* c, const Color::Lab& avg )
{
    float err = 0;
    for( int i=0; i<8; i++ )
    {
        err += sq( c[i].L - avg.L ) + sq( c[i].a - avg.a ) + sq( c[i].b - avg.b );
    }
    return err;
}

static float CalcError( Color::Lab* c, const v3b& average )
{
    return CalcError( c, Color::Lab( average ) );
}

static float CalcError( const uint8* data, const v3b& average )
{
    float err = 0;
    for( int i=0; i<8; i++ )
    {
        uint32 b = *data++;
        uint32 g = *data++;
        uint32 r = *data++;
        err += sq( r - average.x ) + sq( g - average.y ) + sq( b - average.z );
    }
    return err;
}

static inline Color::Lab ToLab( const uint8* data )
{
    uint32 b = *data++;
    uint32 g = *data++;
    uint32 r = *data;
    return Color::Lab( v3b( r, g, b ) );
}

BlockData::BlockData( const BlockBitmapPtr& bitmap, bool perc )
    : m_size( bitmap->Size() )
    , m_perc( perc )
{
    assert( m_size.x%4 == 0 && m_size.y%4 == 0 );

    uint32 cnt = m_size.x * m_size.y / 16;
    DBGPRINT( cnt << " blocks" );
    m_data = new uint64[cnt];

    const uint8* src = bitmap->Data();
    uint64* dst = m_data;

    std::vector<std::future<void>> vec;
    uint32 step = std::max( 1u, cnt / 16 );
    for( uint32 i=0; i<cnt; i+=step )
    {
        vec.push_back( std::async( std::launch::async, [src, dst, step, cnt, i, this]{ ProcessBlocks( src, dst, std::min( step, cnt - i ) ); } ) );

        src += 4*4*3 * step;
        dst += step;
    }
    for( auto& f : vec )
    {
        f.wait();
    }
}

BlockData::~BlockData()
{
    delete[] m_data;
}

BitmapPtr BlockData::Decode()
{
    auto ret = std::make_shared<Bitmap>( m_size );

    uint32* l[4];
    l[0] = ret->Data();
    l[1] = l[0] + m_size.x;
    l[2] = l[1] + m_size.x;
    l[3] = l[2] + m_size.x;

    const uint64* src = (const uint64*)m_data;

    for( int y=0; y<m_size.y/4; y++ )
    {
        for( int x=0; x<m_size.x/4; x++ )
        {
            uint64 d = *src++;

            uint32 r1, g1, b1;
            uint32 r2, g2, b2;

            if( d & 0x2 )
            {
                int32 dr, dg, db;

                r1 = ( d & 0xF8000000 ) >> 27;
                g1 = ( d & 0x00F80000 ) >> 19;
                b1 = ( d & 0x0000F800 ) >> 11;

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

                r2 = r1 + dr;
                g2 = g1 + dg;
                b2 = b1 + db;

                r1 = ( r1 << 3 ) | ( r1 >> 2 );
                g1 = ( g1 << 3 ) | ( g1 >> 2 );
                b1 = ( b1 << 3 ) | ( b1 >> 2 );
                r2 = ( r2 << 3 ) | ( r2 >> 2 );
                g2 = ( g2 << 3 ) | ( g2 >> 2 );
                b2 = ( b2 << 3 ) | ( b2 >> 2 );
            }
            else
            {
                r1 = ( ( d & 0xF0000000 ) >> 24 ) | ( ( d & 0xF0000000 ) >> 28 );
                r2 = ( ( d & 0x0F000000 ) >> 20 ) | ( ( d & 0x0F000000 ) >> 24 );
                g1 = ( ( d & 0x00F00000 ) >> 16 ) | ( ( d & 0x00F00000 ) >> 20 );
                g2 = ( ( d & 0x000F0000 ) >> 12 ) | ( ( d & 0x000F0000 ) >> 16 );
                b1 = ( ( d & 0x0000F000 ) >> 8  ) | ( ( d & 0x0000F000 ) >> 12 );
                b2 = ( ( d & 0x00000F00 ) >> 4  ) | ( ( d & 0x00000F00 ) >> 8  );
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
                    ra = clampu8( r1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ga = clampu8( g1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ba = clampu8( b1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );

                    rb = clampu8( r1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    gb = clampu8( g1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    bb = clampu8( b1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );

                    rc = clampu8( r2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    gc = clampu8( g2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    bc = clampu8( b2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );

                    rd = clampu8( r2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    gd = clampu8( g2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    bd = clampu8( b2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );

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
                    ra = clampu8( r1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ga = clampu8( g1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ba = clampu8( b1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );

                    rb = clampu8( r1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    gb = clampu8( g1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    bb = clampu8( b1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );

                    rc = clampu8( r1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    gc = clampu8( g1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    bc = clampu8( b1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );

                    rd = clampu8( r1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    gd = clampu8( g1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    bd = clampu8( b1 + table[tcw[0]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );

                    *l[0]++ = ra | ( ga << 8 ) | ( ba << 16 ) | 0xFF000000;
                    *l[1]++ = rb | ( gb << 8 ) | ( bb << 16 ) | 0xFF000000;
                    *l[2]++ = rc | ( gc << 8 ) | ( bc << 16 ) | 0xFF000000;
                    *l[3]++ = rd | ( gd << 8 ) | ( bd << 16 ) | 0xFF000000;

                    o += 4;
                }
                for( int i=0; i<2; i++ )
                {
                    ra = clampu8( r2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ga = clampu8( g2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );
                    ba = clampu8( b2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 32 ) ) ) >> ( o + 32 ) ) | ( ( d & ( 1ll << ( o + 48 ) ) ) >> ( o + 47 ) ) ] );

                    rb = clampu8( r2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    gb = clampu8( g2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );
                    bb = clampu8( b2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 33 ) ) ) >> ( o + 33 ) ) | ( ( d & ( 1ll << ( o + 49 ) ) ) >> ( o + 48 ) ) ] );

                    rc = clampu8( r2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    gc = clampu8( g2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );
                    bc = clampu8( b2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 34 ) ) ) >> ( o + 34 ) ) | ( ( d & ( 1ll << ( o + 50 ) ) ) >> ( o + 49 ) ) ] );

                    rd = clampu8( r2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    gd = clampu8( g2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );
                    bd = clampu8( b2 + table[tcw[1]][ ( ( d & ( 1ll << ( o + 35 ) ) ) >> ( o + 35 ) ) | ( ( d & ( 1ll << ( o + 51 ) ) ) >> ( o + 50 ) ) ] );

                    *l[0]++ = ra | ( ga << 8 ) | ( ba << 16 ) | 0xFF000000;
                    *l[1]++ = rb | ( gb << 8 ) | ( bb << 16 ) | 0xFF000000;
                    *l[2]++ = rc | ( gc << 8 ) | ( bc << 16 ) | 0xFF000000;
                    *l[3]++ = rd | ( gd << 8 ) | ( bd << 16 ) | 0xFF000000;

                    o += 4;
                }
            }
        }

        l[0] += m_size.x * 3;
        l[1] += m_size.x * 3;
        l[2] += m_size.x * 3;
        l[3] += m_size.x * 3;
    }

    return ret;
}

static size_t GetLeastError( const float* err, size_t num )
{
    size_t idx = 0;
    for( size_t i=1; i<num; i++ )
    {
        if( err[i] < err[idx] )
        {
            idx = i;
        }
    }
    return idx;
}

static void ProcessAverages( v3b* a )
{
    for( int i=0; i<2; i++ )
    {
        for( int j=0; j<3; j++ )
        {
            int32 c1 = a[i*2][j] >> 3;
            int32 c2 = c1 - ( a[i*2+1][j] >> 3 );
            c2 = std::min( std::max( -4, c2 ), 3 );
            a[4+i*2][j] = ( c1 << 3 ) | ( c1 >> 2 );
            int32 sum = c1 + c2;
            a[5+i*2][j] = ( sum << 3 ) | ( sum >> 2 );
        }
    }
    for( int i=0; i<4; i++ )
    {
        for( int j=0; j<3; j++ )
        {
            a[i][j] &= 0xF0;
            a[i][j] |= a[i][j] >> 4;
        }
    }
}

static void EncodeAverages( uint64& d, const v3b* a, size_t idx )
{
    d |= idx;
    size_t base = idx << 1;

    if( ( idx & 0x2 ) == 0 )
    {
        for( int i=0; i<3; i++ )
        {
            d |= uint64( a[base+0][i] & 0xF0 ) << ( i*8 + 4 );
            d |= uint64( a[base+1][i] & 0xF0 ) << ( i*8 + 8 );
        }
    }
    else
    {
        for( int i=0; i<3; i++ )
        {
            d |= uint64( a[base+0][i] & 0xF8 ) << ( i*8 + 8 );
            int8 c = ( ( a[base+1][i] & 0xF8 ) - ( a[base+0][i] & 0xF8 ) ) >> 3;
            c &= ~0xF8;
            d |= ((uint64)c) << ( i*8 + 8 );
        }
    }
}

static inline size_t GetBufId( size_t i, size_t base )
{
    assert( i < 16 );
    assert( base < 4 );
    if( base % 2 == 0 )
    {
        return base * 2 + 1 - i / 8;
    }
    else
    {
        return base * 2 + 1 - ( ( i / 2 ) % 2 );
    }
}

static uint64 ProcessLab( const uint8* src )
{
    uint64 d = 0;

    Color::Lab b[4][8];
    {
        Color::Lab tmp[16];
        for( int i=0; i<16; i++ )
        {
            tmp[i] = ToLab( src + i*3 );
        }
        size_t s = sizeof( Color::Lab );
        memcpy( b[1], tmp, 8*s );
        memcpy( b[0], tmp + 8, 8*s );
        for( int i=0; i<4; i++ )
        {
            memcpy( b[3]+i*2, tmp+i*4, 2*s );
            memcpy( b[2]+i*2, tmp+i*4+2, 2*s );
        }
    }

    Color::Lab la[4];
    for( int i=0; i<4; i++ )
    {
        la[i] = Average( b[i] );
    }

    v3b a[8];
    for( int i=0; i<4; i++ )
    {
        a[i] = Color::XYZ( la[i] ).RGB();
    }
    ProcessAverages( a );

    float err[4] = { 0 };
    for( int i=0; i<4; i++ )
    {
        err[i/2] += CalcError( b[i], a[i] );
        err[2+i/2] += CalcError( b[i], a[i+4] );
    }
    size_t idx = GetLeastError( err, 4 );

    EncodeAverages( d, a, idx );

    float terr[2][8] = { 0 };
    uint8 tsel[8][16];
    uint8 id[16];
    for( int i=0; i<16; i++ )
    {
        id[i] = (uint8)GetBufId( i, idx );
    }
    const uint8* data = src;
    for( size_t i=0; i<16; i++ )
    {
        uint8 b = *data++;
        uint8 g = *data++;
        uint8 r = *data++;

        Color::Lab lab( v3b( r, g, b ) );

        for( int t=0; t<8; t++ )
        {
            float lerr[4] = { 0 };
            for( int j=0; j<4; j++ )
            {
                v3b crgb;
                for( int k=0; k<3; k++ )
                {
                    crgb[k] = clampu8( a[id[i]][k] + table[t][j] );
                }
                Color::Lab c( crgb );
                lerr[j] += sq( c.L - lab.L ) + sq( c.a - lab.a ) + sq( c.b - lab.b );
            }
            size_t lidx = GetLeastError( lerr, 4 );
            tsel[t][i] = (uint8)lidx;
            terr[id[i]%2][t] += lerr[lidx];
        }
    }
    size_t tidx[2];
    tidx[0] = GetLeastError( terr[0], 8 );
    tidx[1] = GetLeastError( terr[1], 8 );

    d |= tidx[0] << 2;
    d |= tidx[1] << 5;
    for( int i=0; i<16; i++ )
    {
        uint64 t = tsel[tidx[id[i]%2]][i];
        d |= ( t & 0x1 ) << ( i + 32 );
        d |= ( t & 0x2 ) << ( i + 47 );
    }

    return d;
}

static uint64 ProcessRGB( const uint8* src )
{
    uint64 d = 0;

    uint8 b[4][24];

    memcpy( b[1], src, 24 );
    memcpy( b[0], src+24, 24 );

    for( int i=0; i<4; i++ )
    {
        memcpy( b[3]+i*6, src+i*12, 6 );
        memcpy( b[2]+i*6, src+i*12+6, 6 );
    }

    v3b a[8];
    for( int i=0; i<4; i++ )
    {
        a[i] = Average( b[i] );
    }
    ProcessAverages( a );

    float err[4] = { 0 };
    for( int i=0; i<4; i++ )
    {
        err[i/2] += CalcError( b[i], a[i] );
        err[2+i/2] += CalcError( b[i], a[i+4] );
    }
    size_t idx = GetLeastError( err, 4 );

    EncodeAverages( d, a, idx );

    float terr[2][8] = { 0 };
    uint8 tsel[8][16];
    uint8 id[16];
    for( int i=0; i<16; i++ )
    {
        id[i] = (uint8)GetBufId( i, idx );
    }
    const uint8* data = src;
    for( size_t i=0; i<16; i++ )
    {
        uint8 b = *data++;
        uint8 g = *data++;
        uint8 r = *data++;

        for( int t=0; t<8; t++ )
        {
            float lerr[4] = { 0 };
            for( int j=0; j<4; j++ )
            {
                v3b c;
                for( int k=0; k<3; k++ )
                {
                    c[k] = clampu8( a[id[i]][k] + table[t][j] );
                }
                lerr[j] += sq( int32( c.x ) - r ) + sq( int32( c.y ) - g ) + sq( int32( c.z ) - b );
            }
            size_t lidx = GetLeastError( lerr, 4 );
            tsel[t][i] = (uint8)lidx;
            terr[id[i]%2][t] += lerr[lidx];
        }
    }
    size_t tidx[2];
    tidx[0] = GetLeastError( terr[0], 8 );
    tidx[1] = GetLeastError( terr[1], 8 );

    d |= tidx[0] << 2;
    d |= tidx[1] << 5;
    for( int i=0; i<16; i++ )
    {
        uint64 t = tsel[tidx[id[i]%2]][i];
        d |= ( t & 0x1 ) << ( i + 32 );
        d |= ( t & 0x2 ) << ( i + 47 );
    }

    return d;
}

void BlockData::ProcessBlocks( const uint8* src, uint64* dst, uint num )
{
    if( m_perc )
    {
        do
        {
            *dst++ = ProcessLab( src );
            src += 4*4*3;
        }
        while( --num );
    }
    else
    {
        do
        {
            *dst++ = ProcessRGB( src );
            src += 4*4*3;
        }
        while( --num );
    }
}
