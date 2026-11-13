/* Lossless native source pages, independently usable by CPU and GPU consumers.
 * PNG decoding remains stb_image; compact storage replaces repeated full ARGB
 * planes. Incompressible inputs and allocation failures retain the flat path. */
#include <limits.h>
static void image_pages_free(const HdImagePages *pages) {
    if(!pages)return;
    for(uint32_t i=0;i<pages->unique_count;i++)free(pages->unique[i]);
    free(pages->unique);free((void *)pages->map);free((void *)pages);
}
typedef struct {uint64_t hash;uint32_t index;int next;} HdPageHash;
static HdImagePages *compact_image_pages(const uint8_t *rgba,const uint32_t *argb,
                                         unsigned width,unsigned height) {
    uint64_t pixels=(uint64_t)width*height;
    if(pixels<=HD_IMAGE_PAGE_SIZE*HD_IMAGE_PAGE_SIZE || pixels>SIZE_MAX/4)return NULL;
    unsigned columns=(width+127)/128,rows=(height+127)/128;
    size_t count=(size_t)columns*rows;
    if(count>SIZE_MAX/sizeof(uint32_t *) || count>INT_MAX)return NULL;
    HdImagePages *out=calloc(1,sizeof(*out));if(!out)return NULL;
    out->columns=columns;out->rows=rows;
    uint32_t **map=calloc(count,sizeof(*map));out->map=(const uint32_t *const *)map;
    out->unique=calloc(count,sizeof(*out->unique));
    HdPageHash *hashes=malloc(count*sizeof(*hashes));
    if(!map || !out->unique || !hashes){free(hashes);image_pages_free(out);return NULL;}
    int heads[4096];for(unsigned i=0;i<4096;i++)heads[i]=-1;
    uint32_t page[HD_IMAGE_PAGE_SIZE*HD_IMAGE_PAGE_SIZE];
    for(unsigned py=0;py<rows;py++)for(unsigned px=0;px<columns;px++) {
        memset(page,0,sizeof page);
        unsigned w=width-px*128,h=height-py*128;if(w>128)w=128;if(h>128)h=128;
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
            size_t at=(size_t)(py*128+y)*width+px*128+x;
            if(argb)page[y*128+x]=argb[at];
            else {
                const uint8_t *p=rgba+at*4;
                page[y*128+x]=premul(((uint32_t)p[3]<<24)|((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]);
            }
        }
        uint64_t hash=UINT64_C(1469598103934665603);
        for(size_t i=0;i<sizeof page;i+=8){uint64_t word;memcpy(&word,(const uint8_t *)page+i,8);hash=(hash^word)*UINT64_C(1099511628211);}
        unsigned bucket=(unsigned)hash&4095;int found=heads[bucket];
        while(found>=0 && (hashes[found].hash!=hash || memcmp(out->unique[hashes[found].index],page,sizeof page)))found=hashes[found].next;
        uint32_t index;
        if(found>=0)index=hashes[found].index;
        else {
            index=out->unique_count;
            out->unique[index]=malloc(sizeof page);
            if(!out->unique[index]){free(hashes);image_pages_free(out);return NULL;}
            memcpy(out->unique[index],page,sizeof page);out->unique_count++;
            hashes[index]=(HdPageHash){hash,index,heads[bucket]};heads[bucket]=(int)index;
        }
        map[(size_t)py*columns+px]=out->unique[index];
    }
    free(hashes);
    uint64_t compact=(uint64_t)out->unique_count*sizeof page+(uint64_t)count*sizeof(*map)*2+sizeof(*out);
    if(compact>=pixels*4){image_pages_free(out);return NULL;}
    return out;
}
