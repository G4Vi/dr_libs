#ifdef DEBUG
#include <stdio.h>
static void drflac_matroska_print_hex(drflac_uint64 value) {
    drflac_uint8 thebyte;
    int i;
    printf("ebml as hex: ");
    for(i = 0; i < 8; i++) {
        thebyte = value >> (i*8);
        printf("%02X ", thebyte);                
    }
    printf("\n");    
}
#endif

static void drflac_matroska_ebml_push_element_unchecked(ebml_element_reader *reader, drflac_uint64 size, drflac_bool32 isinf) {
    reader->element_inf[reader->depth]  = isinf;
    reader->element_left[reader->depth] = size;
    reader->depth++;
}

static drflac_bool32 drflac_matroska_ebml_size_infinite(drflac_uint64 size, drflac_uint32 size_width) {
    drflac_uint32 bits = (size_width * 7);     
    int i;
    drflac_uint64 infvalue = 0;
    for( i = 0; i < bits; i++) {
        infvalue = (infvalue << 1) | 0x1;
    }
    return  (size == infvalue);
}

static drflac_bool32 drflac_matroska_ebml_push_element(ebml_element_reader *reader, drflac_uint64 size, drflac_uint32 size_width) {
    drflac_bool32 isinf;    

    if(reader->depth == DR_FLAC_MAX_EBML_NEST) return DRFLAC_FALSE;
    if(reader->depth < 1) return DRFLAC_FALSE;
    isinf = drflac_matroska_ebml_size_infinite(size, size_width);

    /* if the parent element isn't infinite there are restrictions on our range */
    if(!reader->element_inf[reader->depth - 1]) {
        if(isinf) return DRFLAC_FALSE;
        if(reader->element_left[reader->depth - 1] < size) return DRFLAC_FALSE;
    }
    
    drflac_matroska_ebml_push_element_unchecked(reader, size, isinf);
    return DRFLAC_TRUE;
}


static drflac_bool32 drflac_matroska_ebml_ok_to_read(ebml_element_reader *reader, drflac_uint64 bytelen) {
    if(reader->element_inf[reader->depth - 1]) return DRFLAC_TRUE;
    if(reader->element_left[reader->depth - 1] >= bytelen) return DRFLAC_TRUE;
    return DRFLAC_FALSE;
}

static void drflac_matroska_ebml_sub_bytes_read(ebml_element_reader *reader, drflac_uint64 bytesread) {
   
    int i;
    for(i = 0; i < reader->depth; i++) {
        /* make no adjustment to infinite elements */
        if(reader->element_inf[i]) continue;
        reader->element_left[i] -= bytesread;
        /* no read to check child elements if this element ended */
        if(reader->element_left[i] == 0) {
            reader->depth = i;
            return;
        }               
    }
}

static size_t drflac__on_read_ebml(void* pUserData, void* bufferOut, size_t bytesToRead) {
    ebml_element_reader *reader = pUserData;
    drflac_uint32 bytes_read = 0;
    
    if(!drflac_matroska_ebml_ok_to_read(reader, bytesToRead)) return 0;

    bytes_read = reader->onRead(reader->pUserData, bufferOut, bytesToRead);
    drflac_matroska_ebml_sub_bytes_read(reader, bytes_read);
    reader->offset += bytes_read;
    return bytes_read;
}

static drflac_bool32 drflac__on_seek_ebml(void* pUserData, int offset, drflac_seek_origin origin) {
    ebml_element_reader *reader = pUserData;
    if(origin == drflac_seek_origin_current) {
        if(!drflac_matroska_ebml_ok_to_read(reader, offset)) return DRFLAC_FALSE;
        if(!reader->onSeek(reader->pUserData, offset, origin)) return DRFLAC_FALSE;
        drflac_matroska_ebml_sub_bytes_read(reader, offset);
        reader->offset += offset;
        return DRFLAC_TRUE;
    }
    return DRFLAC_FALSE;
}

/* used for seeking when we're not allowed to seek such as when reading */ 
static drflac_bool32 drflac_matroska_ebml_read_seek(ebml_element_reader *reader, size_t bytelen) {
    drflac_uint8 element[8];
    size_t toread;
    while(bytelen > 0) {
        toread = (bytelen > 8) ? 8 : bytelen;
        if(drflac__on_read_ebml(reader, element, toread) != toread) return DRFLAC_FALSE;
        bytelen -= toread;
    }
    return DRFLAC_TRUE;
}

static drflac_uint32 drflac_matroska_ebml_vint_width(drflac_uint8 thebyte) {
    drflac_uint32 width;
    for(width = 1; width < 9; width++) {
        if(thebyte & (0x100 >> width)) return width;
    }
    return 0xFFFFFFFF;
}

static drflac_bool32 drflac_matroska_ebml_read_vint(ebml_element_reader *reader, drflac_uint64 *retval, drflac_uint32 *savewidth) {
    drflac_uint32 width;
    drflac_uint64 value = 0;
    drflac_uint8  thebyte;

    drflac_uint32 bytes_read = 0;
    drflac_bool32 ret = DRFLAC_FALSE;	

    if(drflac__on_read_ebml(reader, &thebyte, 1) != 1) return DRFLAC_FALSE;    
    bytes_read++;
    width = drflac_matroska_ebml_vint_width(thebyte);
    if(width == 0xFFFFFFFF) goto drflac_matroska_ebml_read_vint_ret;
    *savewidth = width;

    /* clear VINT_WIDTH and VINT_MARKER */
    thebyte <<= width;
    thebyte >>= width;                                
    /* convert this byte and remaining bytes to an unsigned integer */
    /* https://commandcenter.blogspot.com/2012/04/byte-order-fallacy.html */
    do {
        width--;
        value |= (thebyte << (width*8));
        if(width == 0) {
			*retval = value;
			ret = DRFLAC_TRUE;
            goto drflac_matroska_ebml_read_vint_ret;
		}
		if(drflac__on_read_ebml(reader, &thebyte, 1) != 1) goto drflac_matroska_ebml_read_vint_ret;
        bytes_read++;
    } while(1);

    drflac_matroska_ebml_read_vint_ret:
    return ret;
}



static drflac_bool32 drflac_matroska_ebml_load_element(ebml_element_reader *reader, drflac_uint64 *id) {
    drflac_uint64 size;
    drflac_uint32 width;

    if(!drflac_matroska_ebml_read_vint(reader, id, &width)) return DRFLAC_FALSE;
    if(!drflac_matroska_ebml_read_vint(reader, &size, &width)) return DRFLAC_FALSE;   
    return drflac_matroska_ebml_push_element(reader, size, width);    
}



static drflac_bool32 drflac_matroska_ebml_read_unsigned(ebml_element_reader *reader, size_t bytelen, drflac_uint64 *value) {
    drflac_uint8 element[8];
    int i;
    if(bytelen > sizeof(element)) return DRFLAC_FALSE;

    if(drflac__on_read_ebml(reader, element, bytelen) != bytelen) return DRFLAC_FALSE;	
	
    *value = 0;
    for( i = 0; i < bytelen; i++) {
        *value |= (element[i] << ((bytelen-i-1)*8));
    }
    return DRFLAC_TRUE;
}

static drflac_bool32 drflac_matroska_ebml_read_string(ebml_element_reader *reader, size_t bytelen, drflac_uint8 *string) {
	if(drflac__on_read_ebml(reader, string, bytelen) != bytelen) return DRFLAC_FALSE;
	string[bytelen] = '\0';
    return DRFLAC_TRUE;
}

static drflac_bool32 drflac_matroska_get_flac_codec_privdata(ebml_element_reader *reader) {
    drflac_uint64 id;
    drflac_uint32 size = 0;
    if(reader->depth < 5) {
        while(drflac_matroska_ebml_load_element(reader, &id)) {
            /* Entered Segment */
            if((reader->depth == 2) && (id == 139690087)) continue;
            /* Entered Tracks */
            if((reader->depth == 3) && (id == 106212971)) continue;
            /* Entered Track */
            if((reader->depth == 4) && (id == 46)) continue;
            /* Entered codec priv data */
            if((reader->depth == 5) && (id == 9122)) break;
            size = reader->element_left[reader->depth-1];
            if(!drflac_matroska_ebml_read_seek(reader, size)) return DRFLAC_FALSE; 
        }
    }
    return (reader->depth == 5);   
}

static drflac_bool32 drflac_matroska_read_block(ebml_element_reader *reader, drflac_uint64 blockid) {
    drflac_uint64 trackno;
    drflac_uint32 width;
    drflac_uint64 timestamp;
    drflac_uint64 flag;
    drflac_uint64 frames;
    drflac_uint64 framesize;
    drflac_uint64 frameoffset;
    int i;

    if(!drflac_matroska_ebml_read_vint(reader, &trackno, &width)) return DRFLAC_FALSE;

    /* timestamp s16*/
    if(!drflac_matroska_ebml_read_unsigned(reader, 2, &timestamp)) return DRFLAC_FALSE;
    /* flag */
    if(!drflac_matroska_ebml_read_unsigned(reader, 1, &flag)) return DRFLAC_FALSE;                
    /* no lacing we should be right at a frame*/
    if((flag & 0x6) == 0){
        return DRFLAC_TRUE;
    }
    
    /* number of frames - 1*/
    if(!drflac_matroska_ebml_read_unsigned(reader, 1, &frames)) return DRFLAC_FALSE;

    /* fixed size lacing */
    if((flag & 0x6) == 0x4) return DRFLAC_TRUE;
     
    /* ebml lacing */
    if((flag & 0x6) == 0x6) {
        if(!drflac_matroska_ebml_read_vint(reader, &framesize, &width)) return DRFLAC_FALSE;
        for(i = 0; i < (frames-1); i++) {
            if(!drflac_matroska_ebml_read_vint(reader, &frameoffset, &width)) return DRFLAC_FALSE;
            /* subtract by 2^(bit width-1) - 1 to get the actual integer */
        }
        return DRFLAC_TRUE;                   
    }         

    /* xiph lacing - not implemented*/ 
    return DRFLAC_FALSE; 

}


static drflac_bool32 drflac_matroska_get_block(ebml_element_reader *reader) {
    drflac_uint64 id;
    size_t size;
    if(reader->depth < 4) {
        for(;;) {
            if(!drflac_matroska_ebml_load_element(reader, &id)) break;
            /* Entered Segment */
            if((reader->depth == 2) && (id == 139690087)) continue;
            /* Entered Cluster */
            if((reader->depth == 3) && (id == 256095861)) {
                continue;
            }
            /* Entered Simple block */
            if((reader->depth == 4) && (id == 35)) {
                if(!drflac_matroska_read_block(reader, id)) return DRFLAC_FALSE;
                return (reader->depth == 4);           
            }
            
            /* Entered BlockGroup */
            if((reader->depth == 4) && (id == 32)) {
                continue;
            }
            
            /* Entered Block */
            if((reader->depth == 5) && (id == 33)) {
                if(!drflac_matroska_read_block(reader, id)) return DRFLAC_FALSE;
                return (reader->depth == 5);
            }

            size = reader->element_left[reader->depth-1];
            if(!drflac_matroska_ebml_read_seek(reader, size)) return DRFLAC_FALSE; 
        }

    }     
    return (reader->depth == 4) || (reader->depth == 5);  
}

static drflac_bool32 drflac_matroska_get_currentblock(ebml_element_reader *reader) {
    if(reader->depth == 0) return DRFLAC_FALSE;
    return (reader->offset < reader->flac_priv_size) ? drflac_matroska_get_flac_codec_privdata(reader) : drflac_matroska_get_block(reader);
}


static drflac_bool32 drflac__init_private__matroska(drflac_init_info* pInit, drflac_read_proc onRead, drflac_seek_proc onSeek, drflac_meta_proc onMeta, void* pUserData, void* pUserDataMD, drflac_bool32 relaxed)
{    
    ebml_element_reader reader;
    drflac_uint64 id;
    drflac_uint64 content = 0;
    drflac_uint32 width = 0;
    drflac_uint32 size = 0;
    drflac_uint8  string[9];
    
    drflac_uint32 depth;
    drflac_uint64 lastoffset = 0;
    
    drflac_streaminfo streaminfo;
    drflac_uint8 isLastBlock;
    drflac_uint8 blockType;
    drflac_uint32 blockSize;

    (void)relaxed;
    
    pInit->container = drflac_container_matroska;

    /* setup our ebml stack */
    reader.depth = 0;
    reader.onRead = onRead;
    reader.onSeek = onSeek;
    reader.pUserData = pUserData;
    reader.offset = 0x4;

    reader.segmentoffset = 0;
    reader.tsscale = 1;
    reader.flac_priv_size = 0;    

    drflac_matroska_ebml_push_element_unchecked(&reader, 0, DRFLAC_TRUE);

    
    /*Don't care about master element, just skip past it*/
	if(!drflac_matroska_ebml_read_vint(&reader, &content, &width)) return DRFLAC_FALSE;  
    if(!drflac_matroska_ebml_push_element(&reader, content, width)) return DRFLAC_FALSE;
    if(!drflac_matroska_ebml_read_seek(&reader, content)) return DRFLAC_FALSE;
    if(reader.depth != 1) return DRFLAC_FALSE;    
    
    for(;;) {
        depth = reader.depth;
        lastoffset = reader.offset;
        if(!drflac_matroska_ebml_load_element(&reader, &id)) return DRFLAC_FALSE;
        size = reader.element_left[reader.depth-1];
        /* Segment */
        if((depth == 1) && (id == 139690087)) {
            reader.segmentoffset = lastoffset;
            continue;            
        }
        /* Segment Information*/
        else if((depth == 2) && (id == 88713574)) {
            continue;
        }
        /* timestamp scale */
        else if((depth == 3) && (id == 710577)) {
            if(drflac_matroska_ebml_read_unsigned(&reader, size, &content)) {
                reader.tsscale = content;              
            }                       
        }
        /* Tracks */
        else if((depth == 2) && (id == 106212971)) {
            continue;
        }
        /* Track */
        else if((depth == 3) && (id == 46)) {
            continue;            
        }
        /* Codec priv data */
        else if((depth == 4) && (id == 9122)) {
            if(!drflac_matroska_ebml_read_string(&reader, 4, string)) return DRFLAC_FALSE;
            if((string[0] == 'f') && (string[1] == 'L') && (string[2] == 'a') && (string[3] == 'C')) {
                reader.flac_priv_size = size;
                break;
            }            
        }        
        
        /* skip over segment otherwise */
        size = reader.element_left[reader.depth-1];
        if(!drflac_matroska_ebml_read_seek(&reader, size)) return DRFLAC_FALSE;  
    }

    /* The remaining data in the page should be the STREAMINFO block. */    
    if (!drflac__read_and_decode_block_header(drflac__on_read_ebml, &reader, &isLastBlock, &blockType, &blockSize)) {
        return DRFLAC_FALSE;
    }

    if (blockType != DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO || blockSize != 34) {
        return DRFLAC_FALSE;    /* Invalid block type. First block must be the STREAMINFO block. */
    }
    
    if (!drflac__read_streaminfo(drflac__on_read_ebml, &reader, &streaminfo)) {
        return DRFLAC_FALSE;
    }
    
    /* Success! */
    pInit->hasStreamInfoBlock      = DRFLAC_TRUE;
    pInit->sampleRate              = streaminfo.sampleRate;
    pInit->channels                = streaminfo.channels;
    pInit->bitsPerSample           = streaminfo.bitsPerSample;
    pInit->totalPCMFrameCount      = streaminfo.totalPCMFrameCount;
    pInit->maxBlockSizeInPCMFrames = streaminfo.maxBlockSizeInPCMFrames;
    pInit->hasMetadataBlocks       = !isLastBlock;

    if (onMeta) {
        drflac_metadata metadata;
        metadata.type = DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO;
        metadata.pRawData = NULL;
        metadata.rawDataSize = 0;
        metadata.data.streaminfo = streaminfo;
        onMeta(pUserDataMD, &metadata);
    }

    pInit->matroskaReader = reader;
    return DRFLAC_TRUE;

    /* Tags  39109479 */
    /* Cluster 256095861 */
      /* CRC 63*/
      /* cluster timestamp 103  */
      /* Simple block 35*/
}

typedef struct {
    drflac_uint8 cache[1048576];
    ebml_element_reader reader;
    size_t bytes_in_cache;    
} drflac_matroskabs;

static size_t drflac__on_read_matroska(void* pUserData, void* bufferOut, size_t bytesToRead) {
    drflac_uint8 *bufferbytes = bufferOut;
    drflac_matroskabs *bs = (drflac_matroskabs *)pUserData;
    size_t tocopy;
    size_t bytesread = 0;
    size_t currentread;    

    while(bytesToRead > 0) {
        if(!drflac_matroska_get_currentblock(&bs->reader)) {
            return  bytesread;
        }        
          
        /* copy data from the frame */
        tocopy = bs->reader.element_left[bs->reader.depth-1] < bytesToRead ? bs->reader.element_left[bs->reader.depth-1] : bytesToRead;
        currentread = drflac__on_read_ebml(&bs->reader, bufferbytes+bytesread, tocopy);
        bytesread += currentread;
        if(currentread != tocopy) {
            return bytesread;
        } 
        bytesToRead -= tocopy;
    }    
    return bytesread;   
}

static drflac_bool32 drflac__on_seek_matroska(void* pUserData, int offset, drflac_seek_origin origin)
{
    drflac_matroskabs * matroskabs = (drflac_matroskabs*)pUserData;
    int toseek;

    DRFLAC_ASSERT(matroskabs != NULL);
    DRFLAC_ASSERT(offset >= 0);  /* <-- Never seek backwards. */
    if(origin == drflac_seek_origin_start) {
        /* physical seek to segment */
        if(!matroskabs->reader.onSeek(matroskabs->reader.pUserData, matroskabs->reader.segmentoffset, drflac_seek_origin_start)) return DRFLAC_FALSE;
        matroskabs->reader.offset = matroskabs->reader.segmentoffset;
        matroskabs->reader.depth = 1;
              
        return drflac__on_seek_matroska(matroskabs, offset, drflac_seek_origin_current);
    }
    DRFLAC_ASSERT(origin == drflac_seek_origin_current);    

    while(offset > 0) {
        if(!drflac_matroska_get_currentblock(&matroskabs->reader)) return DRFLAC_FALSE;
        toseek = offset > matroskabs->reader.element_left[matroskabs->reader.depth-1] ? matroskabs->reader.element_left[matroskabs->reader.depth-1] : offset;
        if(!drflac__on_seek_ebml(&matroskabs->reader, toseek, drflac_seek_origin_current)) return DRFLAC_FALSE;
        offset -= toseek;
    }   

    return DRFLAC_TRUE;
}

static drflac_uint64 drflac_matroska_round(double d) {
    return (drflac_uint64)(d + 0.5);
}

static drflac_bool32 drflac_matroska__seek_to_pcm_frame(drflac* pFlac, drflac_uint64 pcmFrameIndex) {
    
    ebml_element_reader *reader = &((drflac_matroskabs *)pFlac->bs.pUserData)->reader;
    
    drflac_uint64 id;
    drflac_uint64 ts;
    #ifdef DEBUG    
    drflac_uint64 beforeframe;
    #endif
    drflac_uint64 curframe;

    drflac_uint64 runningPCMFrameCount;
    drflac_uint64 cluster = 0;
    drflac_uint64 element_left[4];
    drflac_bool32 element_inf[4];

    /* first seek to Segment */
    if(!reader->onSeek(reader->pUserData, reader->segmentoffset, drflac_seek_origin_start))  return DRFLAC_FALSE;
    reader->depth = 1;
    reader->offset = reader->segmentoffset;    

    /* seek into the cluster that might have our frame */
    DRFLAC_ASSERT(reader->depth == 1);
    for(;;) {        
        if(!drflac_matroska_ebml_load_element(reader, &id)) break;

        /* Segment */
        if((reader->depth == 2) && (id == 139690087)) {
            continue;            
        }

        /* Entered Cluster */
        if((reader->depth == 3) && (id == 256095861)) {       
            continue;
        }           
        /* Found a Cluster timestamp */
        if((reader->depth == 4) && (id == 103)) {          
            if(!drflac_matroska_ebml_read_unsigned(reader, reader->element_left[reader->depth-1], &ts)) return DRFLAC_FALSE;
            curframe = drflac_matroska_round((ts * reader->tsscale)  * (double)pFlac->sampleRate / 1000000000);

            /* Hack, adjust curframe when the tsscale isn't good enough*/
            if(reader->tsscale > (1000000000 / pFlac->sampleRate)) {
                #ifdef DEBUG
                beforeframe = curframe;
                #endif
                curframe = drflac_matroska_round((double)curframe/4096)*4096;          
            }
            
            /* this cluster could contain our frame, save it's info*/
            if(curframe <= pcmFrameIndex) {
                element_left[reader->depth-3] = reader->element_left[reader->depth-3];     
                element_inf[reader->depth-3] = reader->element_inf[reader->depth-3];
                element_left[reader->depth-2] = reader->element_left[reader->depth-2];     
                element_inf[reader->depth-2] = reader->element_inf[reader->depth-2];
                element_left[reader->depth-1] = reader->element_left[reader->depth-1];     
                element_inf[reader->depth-1] = reader->element_inf[reader->depth-1];
                runningPCMFrameCount = curframe;
                cluster = reader->offset;
                if(curframe == pcmFrameIndex) break;
            }
            /* this is past the frame, breakout */                
            else if(curframe > pcmFrameIndex){
                break;
            }            
        }

        /* todo use faster seek op ? */
        if(!drflac_matroska_ebml_read_seek(reader, reader->element_left[reader->depth-1])) return DRFLAC_FALSE; 
    }
    if(cluster == 0) return DRFLAC_FALSE;    
    if(!reader->onSeek(reader->pUserData, cluster, drflac_seek_origin_start))  return DRFLAC_FALSE;
    reader->offset = cluster;
    reader->depth = 3;
    reader->element_left[0] = element_left[0];
    reader->element_inf[0] = element_inf[0];
    reader->element_left[1] = element_left[1];
    reader->element_inf[1] = element_inf[1];
    reader->element_left[2] = element_left[2];
    reader->element_inf[2] = element_inf[2];   
    
    for (;;) {
        /*
        Use the native FLAC decoder for the rest of seeking for simplicity.
        This is the same code from the bottom of drflac_ogg__seek_to_pcm_frame.
        */

        drflac_uint64 firstPCMFrameInFLACFrame = 0;
        drflac_uint64 lastPCMFrameInFLACFrame = 0;
        drflac_uint64 pcmFrameCountInThisFrame;

        if (!drflac__read_next_flac_frame_header(&pFlac->bs, pFlac->bitsPerSample, &pFlac->currentFLACFrame.header)) {
            return DRFLAC_FALSE;
        }

        drflac__get_pcm_frame_range_of_current_flac_frame(pFlac, &firstPCMFrameInFLACFrame, &lastPCMFrameInFLACFrame);

        pcmFrameCountInThisFrame = (lastPCMFrameInFLACFrame - firstPCMFrameInFLACFrame) + 1;

        /* If we are seeking to the end of the file and we've just hit it, we're done. */
        if (pcmFrameIndex == pFlac->totalPCMFrameCount && (runningPCMFrameCount + pcmFrameCountInThisFrame) == pFlac->totalPCMFrameCount) {
            drflac_result result = drflac__decode_flac_frame(pFlac);
            if (result == DRFLAC_SUCCESS) {
                pFlac->currentPCMFrame = pcmFrameIndex;
                pFlac->currentFLACFrame.pcmFramesRemaining = 0;
                return DRFLAC_TRUE;
            } else {
                return DRFLAC_FALSE;
            }
        }

        if (pcmFrameIndex < (runningPCMFrameCount + pcmFrameCountInThisFrame)) {
            /*
            The sample should be in this FLAC frame. We need to fully decode it, however if it's an invalid frame (a CRC mismatch), we need to pretend
            it never existed and keep iterating.
            */
            drflac_result result = drflac__decode_flac_frame(pFlac);
            if (result == DRFLAC_SUCCESS) {
                /* The frame is valid. We just need to skip over some samples to ensure it's sample-exact. */
                drflac_uint64 pcmFramesToDecode = (size_t)(pcmFrameIndex - runningPCMFrameCount);    /* <-- Safe cast because the maximum number of samples in a frame is 65535. */
                if (pcmFramesToDecode == 0) {
                    return DRFLAC_TRUE;
                }

                pFlac->currentPCMFrame = runningPCMFrameCount;

                return drflac__seek_forward_by_pcm_frames(pFlac, pcmFramesToDecode) == pcmFramesToDecode;  /* <-- If this fails, something bad has happened (it should never fail). */
            } else {
                if (result == DRFLAC_CRC_MISMATCH) {
                    continue;   /* CRC mismatch. Pretend this frame never existed. */
                } else {
                    return DRFLAC_FALSE;
                }
            }
        } else {
            /*
            It's not in this frame. We need to seek past the frame, but check if there was a CRC mismatch. If so, we pretend this
            frame never existed and leave the running sample count untouched.
            */
            drflac_result result = drflac__seek_to_next_flac_frame(pFlac);
            if (result == DRFLAC_SUCCESS) {
                runningPCMFrameCount += pcmFrameCountInThisFrame;
            } else {
                if (result == DRFLAC_CRC_MISMATCH) {
                    continue;   /* CRC mismatch. Pretend this frame never existed. */
                } else {
                    return DRFLAC_FALSE;
                }
            }
        }
    }    
}