/*******************************************************************************
 * Copyright (c) 2016, SoftPLC Corportion.
 *
 *
 ******************************************************************************/

#include <string.h>
#include <stdarg.h>

#include "cipster_api.h"
#include "cipepath.h"
#include "byte_bufs.h"


//* @brief Enum containing values which kind of logical segment is encoded
enum LogicalSegmentType
{
    kLogicalSegmentTypeClassId         = 0x00 + kSegmentTypeLogical,    ///< Class ID
    kLogicalSegmentTypeInstanceId      = 0x04 + kSegmentTypeLogical,    ///< Instance ID
    kLogicalSegmentTypeMemberId        = 0x08 + kSegmentTypeLogical,    ///< Member ID
    kLogicalSegmentTypeConnectionPoint = 0x0C + kSegmentTypeLogical,    ///< Connection Point
    kLogicalSegmentTypeAttributeId     = 0x10 + kSegmentTypeLogical,    ///< Attribute ID
    kLogicalSegmentTypeSpecial         = 0x14 + kSegmentTypeLogical,    ///< Special
    kLogicalSegmentTypeService         = 0x18 + kSegmentTypeLogical,    ///< Service ID
    kLogicalSegmentTypeExtendedLogical = 0x1C + kSegmentTypeLogical,    ///< Extended Logical
};


enum NetworkSegmentSubType
{
    kProductionTimeInhibitTimeNetworkSegment = 0x43 ///< production inhibit time network segment
};


enum DataSegmentType
{
    kDataSegmentTypeSimpleDataMessage         = kSegmentTypeData + 0x00,
    kDataSegmentTypeAnsiExtendedSymbolMessage = kSegmentTypeData + 0x11
};



CipAppPath& CipAppPath::operator = ( const CipAppPath& other )
{
    pbits = other.pbits;

    if( HasLogical() )
        memcpy( stuff, other.stuff, sizeof stuff );

    if( HasSymbol() )
        memcpy( tag, other.tag, sizeof tag );
}


bool CipAppPath::operator == ( const CipAppPath& other ) const
{
    if( pbits != other.pbits )
        return false;

    if( HasClass() && stuff[CLASS] != other.stuff[CLASS] )
        return false;

    if( HasInstance() && stuff[INSTANCE] != other.stuff[INSTANCE] )
        return false;

    if( HasAttribute() && stuff[ATTRIBUTE] != other.stuff[ATTRIBUTE] )
        return false;

    if( HasConnPt() && stuff[CONN_PT] != other.stuff[CONN_PT] )
        return false;

    if( HasSymbol() && strcmp( tag, other.tag ) )
        return false;

    return true;
}


bool CipAppPath::SetSymbol( const char* aSymbol )
{
    int slen = strlen( aSymbol );
    if( slen > (int) sizeof(tag) - 1 )
        return false;
    strcpy( tag, aSymbol );

    pbits |= (1<<TAG);

    return true;
}


const char* CipAppPath::GetSymbol() const
{
    return HasSymbol() ? tag : "";
}


CipClass* CipAppPath::Class() const
{
    return GetCipClass( GetClass() );
}


CipInstance* CipAppPath::Instance() const
{
    CipClass* clazz = GetCipClass( GetClass() );
    if( clazz )
    {
        return clazz->Instance( GetInstanceOrConnPt() );
    }
    return NULL;
}


CipAttribute* CipAppPath::Attribute( int aAttrId ) const
{
    CipInstance* instance = Instance();
    if( instance )
    {
        return instance->Attribute( aAttrId );
    }
    return NULL;
}


static void serialize( BufWriter& out, int seg_type, unsigned aValue )
{
    if( aValue < 256 )
    {
        *out++ = seg_type;
        *out++ = aValue;
    }
    else if( aValue < 65536 )
    {
        *out++ = seg_type | 1;
        out.put16( aValue );
    }
    else
    {
        *out++ = seg_type | 2;
        out.put32( aValue );
    }
}


int CipAppPath::SerializeAppPath( BufWriter aOutput )
{
    BufWriter out = aOutput;

    if( HasSymbol() )
    {
        int tag_count = strlen( tag );

        *out++ = kDataSegmentTypeAnsiExtendedSymbolMessage;
        *out++ = tag_count;

        out.append( (EipByte*) tag, tag_count );

        if( (out.data() - aOutput.data()) & 1 )
            *out++ = 0;               // output possible pad byte

        if( HasConnPt() )
            serialize( out, kLogicalSegmentTypeConnectionPoint, GetConnPt() );

        if( HasMember1() )
        {
            serialize( out, kLogicalSegmentTypeMemberId, GetMember1() );

            if( HasMember2() )
            {
                serialize( out, kLogicalSegmentTypeMemberId, GetMember2() );

                if( HasMember3() )
                {
                    serialize( out, kLogicalSegmentTypeMemberId, GetMember3() );
                }
            }
        }
    }

    else    // is logical
    {
        if( HasClass() )
            serialize( out, kLogicalSegmentTypeClassId, GetClass() );

        if( HasInstance() )
            serialize( out, kLogicalSegmentTypeInstanceId, GetInstance() );

        if( HasAttribute() )
            serialize( out, kLogicalSegmentTypeAttributeId, GetAttribute() );

        if( HasConnPt() )
            serialize( out, kLogicalSegmentTypeConnectionPoint, GetConnPt() );
    }

    return out.data() - aOutput.data();
}


int CipAppPath::deserialize_logical( BufReader aInput, CipAppPath::Stuff aField, int aFormat )
{
    CIPSTER_ASSERT( aFormat >= 0 && aFormat <= 2 );
    CIPSTER_ASSERT( aField==MEMBER1 || aField==MEMBER2 || aField==MEMBER3 ||
        aField==CONN_PT || aField==ATTRIBUTE || aField==INSTANCE || aField==CLASS );

    BufReader in = aInput;

    int value;

    if( aFormat == 0 )
        value = *in++;
    else if( aFormat == 1 )
    {
        ++in;
        value = in.get16();
    }
    else if( aFormat == 2 )
    {
        ++in;
        value = in.get32();
    }

    stuff[aField] = value;
    pbits |= 1 << aField;

    return in.data() - aInput.data();
}


inline int CipAppPath::deserialize_symbolic( BufReader aInput )
{
    BufReader in = aInput;

    int first = *in;

    if( first == kDataSegmentTypeAnsiExtendedSymbolMessage )
    {
        ++in;     // ate first

        int byte_count = *in++;

        if( byte_count > (int) sizeof(tag)-1 )
        {
            return -1;
        }

        memcpy( tag, in.data(), byte_count );
        tag[byte_count] = 0;
        in += byte_count;

        if( (in.data() - aInput.data()) & 1 )
            ++in;    // consume pad byte if any

        pbits |= (1<<TAG);
    }

    else if( (first & 0xe0) == kSegmentTypeSymbolic )   // Symbolic Segment
    {
        // cannot exceed 31 which is less than max of nul terminated 'this->tag'
        int symbol_size = first & 0x1f;

        if( symbol_size == 0 )
        {
            CIPSTER_TRACE_ERR( "%s: saw unsupported 'extended' Symbolic Segment\n", __func__ );

            return -1;
        }

        ++in;    // ate first

        memcpy( tag, in.data(), symbol_size );
        tag[symbol_size] = 0;
        in += symbol_size;

        if( (in.data() - aInput.data()) & 1 )
            ++in;              // skip pad byte if any

        pbits |= (1<<TAG);
    }

    return in.data() - aInput.data();
}


int CipAppPath::DeserializeAppPath( BufReader aInput, CipAppPath* aPreviousToInheritFrom )
{
    BufReader in = aInput;
                                                // is seen, C-1.6 of Vol1_3.19
    Clear();

    int result = deserialize_symbolic( in );

    if( result < 0 )
        return result;

    if( result > 0 )
    {
        in += result;       // advance over deserialized symbolic

        int first = *in;

        // Grammar in C.1.5 shows we can have Connection_Point optionally here:
        if( (first & 0xfc) == kLogicalSegmentTypeConnectionPoint )
        {
            ++in;
            in += deserialize_logical( in, CONN_PT, first & 3 );
        }

        first = *in;

        Stuff   last_member;

        for( last_member = MEMBER1;  last_member <= MEMBER3; last_member = Stuff(last_member +1) )
        {
            /*

                Grammar in C.1.5 shows we can have member_specification optionally here.

                member id is the "element id" found in A-B publication
                "Logix5000 Data Access" (Rockwell Automation Publication
                1756-PM020D-EN-P - June 2016) and is expected only in the
                context of a symbolic address accoring to that document.

            */
            if( (first & 0xfc) == kLogicalSegmentTypeMemberId )
            {

                ++in;
                in += deserialize_logical( in, last_member, first & 3 );
            }
            else
                break;
        }
    }

    else    // check for logical rather than symbolic
    {
        Stuff   last_member = LOGICAL_END;      // exit loop when higher member

        while( in.size() )
        {
            int first = *in;

            int seg_type = 0xfc & first;
            int format   = 0x03 & first;

            Stuff   next;

            switch( seg_type )
            {
            case kLogicalSegmentTypeClassId:            next = CLASS;       break;
            case kLogicalSegmentTypeInstanceId:         next = INSTANCE;    break;
            case kLogicalSegmentTypeAttributeId:        next = ATTRIBUTE;   break;
            case kLogicalSegmentTypeConnectionPoint:    next = CONN_PT;     break;

            default:
                // C-1.6 of Vol1_3.19; is an expected termination point, not an error.
                goto logical_exit;
            }

            // The grammar shown in Vol1 C-1.5 shows that assembly_class_application_path
            // is weird in that it can only take INSTANCE or CONN_PT not both.  So when we
            // see these back to back, this is an app_path boundary.
            if( GetClass() == 4 && last_member == INSTANCE && next == CONN_PT )
            {
                goto logical_exit;
            }

            if( next >= last_member )
            {
                // C-1.6 of Vol1_3.19; is an expected termination point, not an error.
                goto logical_exit;
            }

            ++in;  // ate first

            in += deserialize_logical( in, next, format );

            last_member = next;
        }

logical_exit:
        if( in.data() > aInput.data() && aPreviousToInheritFrom )
        {
            if( aPreviousToInheritFrom->GetClass() == 4 )
                inherit_assembly( last_member + 1, aPreviousToInheritFrom );
            else
                inherit( last_member + 1, aPreviousToInheritFrom );
        }
    }

exit:
    int byte_count = in.data() - aInput.data();

    return byte_count;
}


static int vprint( std::string* result, const char* format, va_list ap )
{
    char    msg[512];

    size_t  len = vsnprintf( msg, sizeof(msg), format, ap );

    if( len < sizeof(msg) )     // the output fit into msg
    {
        result->append( msg, msg + len );
    }

    return len;
}


std::string Sprintf( const char* aFormat, ... )
{
    std::string ret;
    va_list     args;

    va_start( args, aFormat );
    int ignore = vprint( &ret, aFormat, args );
    (void) ignore;
    va_end( args );

    return ret;
}


const std::string CipAppPath::Format() const
{
    std::string ret;

    if( HasClass() )
    {
        ret += "Class:";
        ret += Sprintf( "%d", GetClass() );

        if( HasInstance() )
        {
            ret += " Instance:";
            ret += Sprintf( "%d", GetInstance() );
        }

        if( HasConnPt() )
        {
            ret += " ConnPt:";
            ret += Sprintf( "%d", GetConnPt() );
        }
    }
    else if( HasSymbol() )
    {
        ret += "Tag:";
        ret += tag;

        if( HasMember1() )
        {
            ret += Sprintf( "[%d]", GetMember1() );

            if( HasMember2() )
            {
                ret += Sprintf( "[%d]", GetMember2() );

                if( HasMember3() )
                    ret += Sprintf( "[%d]", GetMember3() );
            }
        }
    }

    return ret;
}


void CipAppPath::inherit( int aStart, CipAppPath* aPreviousToInheritFrom )
{
    CIPSTER_ASSERT( aPreviousToInheritFrom );

    for( int i=aStart;  i < LOGICAL_END;  ++i )
    {
        if( !( pbits & (1<<i) ) && ( aPreviousToInheritFrom->pbits & (1<<i) ) )
        {
            stuff[i] = aPreviousToInheritFrom->stuff[i];
            pbits |= (1<<i);
        }
    }
}


void CipAppPath::inherit_assembly( int aStart, CipAppPath* aPreviousToInheritFrom )
{
    CIPSTER_ASSERT( aPreviousToInheritFrom );

    for( int i=aStart;  i < LOGICAL_END;  ++i )
    {
        if( aStart == INSTANCE && i == INSTANCE )
            continue;

        if( !( pbits & (1<<i) ) && ( aPreviousToInheritFrom->pbits & (1<<i) ) )
        {
            stuff[i] = aPreviousToInheritFrom->stuff[i];
            pbits |= (1<<i);
        }
    }
}


static int parsePortSegment( BufReader aInput, CipPortSegment* aSegment )
{
    BufReader   in = aInput;
    int         first = *in++;

    // p points to 2nd byte here.
    int link_addrz = (first & 0x10) ? *in++ : 0;

    if( (first & 0xf) == 15 )
        aSegment->port = in.get16();
    else
        aSegment->port = first & 0xf;

    aSegment->link_address.clear();

    while( link_addrz-- )
    {
        aSegment->link_address.push_back( *in++ );
    }

    // skip a byte if not an even number of them have been consumed.
    if( (in.data() - aInput.data()) & 1 )
        ++in;

    return in.data() - aInput.data();
}


int CipPortSegmentGroup::DeserializePortSegmentGroup( BufReader aInput )
{
    BufReader in = aInput;

    EipUint32   value;

    Clear();

    while( in.size() )
    {
        int first = *in;

        if( (first & 0xe0) == kSegmentTypePort )
        {
            in += parsePortSegment( in, &port );
            pbits |= (1<<PORT);
        }
        else
        {
            switch( first )
            {
            case 0x34:      // electronic key
                ++in;

                int key_format;

                key_format = *in++;

                if( key_format != 4 )
                {
                    CIPSTER_TRACE_ERR( "%s: unknown electronic key format: %d\n",
                        __func__, key_format );

                    return aInput.data() - (in.data() - 1);    // return negative byte offset of error
                }

                key.vendor_id      = in.get16();
                key.device_type    = in.get16();
                key.product_code   = in.get16();
                key.major_revision = *in++;
                key.minor_revision = *in++;
                pbits |= (1<<KEY);
                break;

            case 0x43:
                ++in;
                value = *in++;
                SetPIT_MSecs( value );  // convert to usecs
                break;

            case 0x51:
                ++in;

                int num_words;
                num_words = *in++;

                if( num_words == 1 )
                {
                    value = in.get16();
                }
                else if( num_words == 2 )
                {
                    value = in.get32();
                }
                else
                {
                    CIPSTER_TRACE_ERR( "%s: unknown PIT_USECS format: %d\n", __func__, num_words );
                    return aInput.data() - (in.data() - 1);    // return negative byte offset of error
                }
                SetPIT_USecs( value );
                break;

            default:
                goto exit;
            }
        }
    }

exit:
    return in.data() - aInput.data();
}


int CipSimpleDataSegment::DeserializeDataSegment( BufReader aInput )
{
    BufReader in = aInput;

    Clear();

    int first = *in++;

    if( first == kDataSegmentTypeSimpleDataMessage )
    {
        int word_count = *in++;

        while( word_count-- )
        {
            CipWord w = in.get16();
            words.push_back( w );
        }

        pbits |= 1; // caller can use HasAny()
    }

    return in.data() - aInput.data();
}

