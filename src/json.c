#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

// Everything the document owns -- values, keys, decoded strings -- is bump
// allocated out of one chain of blocks, so json_free is a walk of that chain
// instead of a recursive tree walk, and no pointer we hand out can be freed
// twice or dangle before json_free.
//
// Container children are gathered on one shared parse stack rather than a list
// per container: a container's children always end up contiguous at the top of
// that stack (a nested container pops its own children before pushing itself),
// so on the closing brace they are one memcpy away from a contiguous arena
// array. That keeps json_at O(1) without a second pass over the text.

#define MAX_DEPTH  200          // adversarial nesting must error, not smash the stack
#define BLOCK_MIN  4096u
#define BLOCK_MAX  (1u<<20)

typedef struct Block{
    struct Block* next;
    size_t used;
    size_t cap;
    unsigned char data[];
} Block;

struct JsonValue{
    JsonType type;
    const char* key;    // member name when this value sits inside an object, else NULL
    union{
        bool b;
        double num;
        const char* str;
        struct{ JsonValue* items; size_t count; } list;  // array elements or object members
    } as;
};

struct JsonDoc{
    Block* blocks;
    size_t next_cap;
    JsonValue root;
};

typedef struct Parser{
    const char* s;
    size_t len;
    size_t pos;
    JsonDoc* doc;
    JsonValue* stack;   // children of every container currently open
    size_t stack_len;
    size_t stack_cap;
    char* sbuf;         // decoded string bytes / NUL-terminated number text
    size_t sbuf_len;
    size_t sbuf_cap;
    int depth;
    char* errbuf;
    size_t errbuf_len;
} Parser;

// ---- errors -----------------------------------------------------------------

static bool fail(Parser* p,size_t pos,const char* msg){
if(p->errbuf&&p->errbuf_len)snprintf(p->errbuf,p->errbuf_len,"%s at byte %zu",msg,pos);
return false;
}

// same, but naming the offending byte -- an unprintable one as hex, since the
// message ends up in a terminal.
static bool fail_byte(Parser* p,size_t pos,const char* what){
char m[96];
if(pos>=p->len)snprintf(m,sizeof m,"%s but hit end of input",what);
else{
    unsigned char c=(unsigned char)p->s[pos];
    if(c>=0x20&&c<0x7F)snprintf(m,sizeof m,"%s, found '%c'",what,(char)c);
    else snprintf(m,sizeof m,"%s, found byte 0x%02X",what,(unsigned)c);
}
return fail(p,pos,m);
}

// ---- arena ------------------------------------------------------------------

static size_t align_pad(const unsigned char* at,size_t align){
uintptr_t a=(uintptr_t)align;
uintptr_t base=(uintptr_t)at;
return (size_t)((a-(base%a))%a);
}

// malloc gives back max-aligned memory but sizeof(Block) need not be a multiple
// of that alignment, so pad from the absolute address rather than trusting the
// offset of the flexible member.
static void* arena_alloc(JsonDoc* doc,size_t size,size_t align){
if(size==0)size=1;
if(size>SIZE_MAX-align-sizeof(Block))return NULL;
Block* b=doc->blocks;
if(b){
    unsigned char* base=b->data+b->used;
    size_t pad=align_pad(base,align);
    size_t left=b->cap-b->used;
    if(left>=pad&&left-pad>=size){ b->used+=pad+size; return base+pad; }
}
size_t need=size+align;
size_t cap=doc->next_cap<need?need:doc->next_cap;
Block* nb=malloc(sizeof(Block)+cap);
if(!nb)return NULL;
nb->next=doc->blocks;
nb->cap=cap;
nb->used=0;
doc->blocks=nb;
if(doc->next_cap<BLOCK_MAX)doc->next_cap*=2;
size_t pad=align_pad(nb->data,align);
nb->used=pad+size;
return nb->data+pad;
}

static const char* arena_str(Parser* p,const char* s,size_t n){
char* d=arena_alloc(p->doc,n+1,1);
if(!d)return NULL;
if(n)memcpy(d,s,n);
d[n]='\0';
return d;
}

// ---- growable scratch -------------------------------------------------------

static bool stack_push(Parser* p,const JsonValue* v){
if(p->stack_len==p->stack_cap){
    size_t cap=p->stack_cap?p->stack_cap*2:64;
    if(cap>SIZE_MAX/sizeof(JsonValue))return false;
    JsonValue* n=realloc(p->stack,cap*sizeof(JsonValue));
    if(!n)return false;
    p->stack=n;
    p->stack_cap=cap;
}
p->stack[p->stack_len++]=*v;
return true;
}

static bool sbuf_push(Parser* p,const char* bytes,size_t n){
if(n>SIZE_MAX-p->sbuf_len)return false;
if(p->sbuf_len+n>p->sbuf_cap){
    size_t cap=p->sbuf_cap?p->sbuf_cap:128;
    while(cap<p->sbuf_len+n){ if(cap>SIZE_MAX/2)return false; cap*=2; }
    char* nb=realloc(p->sbuf,cap);
    if(!nb)return false;
    p->sbuf=nb;
    p->sbuf_cap=cap;
}
if(n)memcpy(p->sbuf+p->sbuf_len,bytes,n);
p->sbuf_len+=n;
return true;
}

// ---- scanning ---------------------------------------------------------------

static bool is_digit(char c){ return c>='0'&&c<='9'; }

static void skip_ws(Parser* p){
// RFC 8259 whitespace only: a stray vertical tab or form feed is an error, not
// a separator.
while(p->pos<p->len){
    char c=p->s[p->pos];
    if(c==' '||c=='\t'||c=='\n'||c=='\r')p->pos++;
    else break;
}
}

static bool hex4(Parser* p,size_t at,uint32_t* out){
if(at>p->len||p->len-at<4)return false;
uint32_t v=0;
for(size_t i=0;i<4;i++){
    unsigned char c=(unsigned char)p->s[at+i];
    uint32_t d;
    if(c>='0'&&c<='9')d=(uint32_t)(c-'0');
    else if(c>='a'&&c<='f')d=(uint32_t)(c-'a'+10);
    else if(c>='A'&&c<='F')d=(uint32_t)(c-'A'+10);
    else return false;
    v=(v<<4)|d;
}
*out=v;
return true;
}

static bool push_utf8(Parser* p,uint32_t cp){
char b[4];
size_t n;
if(cp<0x80u){ b[0]=(char)cp; n=1; }
else if(cp<0x800u){ b[0]=(char)(0xC0u|(cp>>6)); b[1]=(char)(0x80u|(cp&0x3Fu)); n=2; }
else if(cp<0x10000u){ b[0]=(char)(0xE0u|(cp>>12)); b[1]=(char)(0x80u|((cp>>6)&0x3Fu)); b[2]=(char)(0x80u|(cp&0x3Fu)); n=3; }
else{ b[0]=(char)(0xF0u|(cp>>18)); b[1]=(char)(0x80u|((cp>>12)&0x3Fu)); b[2]=(char)(0x80u|((cp>>6)&0x3Fu)); b[3]=(char)(0x80u|(cp&0x3Fu)); n=4; }
return sbuf_push(p,b,n);
}

// Decodes the string starting at the current '"' into p->sbuf. The decoded
// bytes are not terminated here (a \u0000 escape decodes to a real NUL), so
// arena_str appends the terminator when the bytes are interned.
static bool parse_string_raw(Parser* p){
size_t open=p->pos;
p->pos++;
p->sbuf_len=0;
for(;;){
    // copy the escape-free stretch in one go; most keys and names have no
    // escapes at all.
    size_t run=p->pos;
    while(p->pos<p->len){
        unsigned char c=(unsigned char)p->s[p->pos];
        if(c=='"'||c=='\\'||c<0x20)break;
        p->pos++;
    }
    if(p->pos>run&&!sbuf_push(p,p->s+run,p->pos-run))return fail(p,run,"out of memory");
    if(p->pos>=p->len)return fail(p,open,"unterminated string");
    unsigned char c=(unsigned char)p->s[p->pos];
    if(c=='"'){ p->pos++; return true; }
    if(c<0x20)return fail(p,p->pos,"unescaped control character in string");

    size_t esc=p->pos;
    p->pos++;
    if(p->pos>=p->len)return fail(p,esc,"unterminated escape");
    char e=p->s[p->pos++];
    char lit;
    switch(e){
    case '"':  lit='"';  break;
    case '\\': lit='\\'; break;
    case '/':  lit='/';  break;
    case 'b':  lit='\b'; break;
    case 'f':  lit='\f'; break;
    case 'n':  lit='\n'; break;
    case 'r':  lit='\r'; break;
    case 't':  lit='\t'; break;
    case 'u':{
        uint32_t cp;
        if(!hex4(p,p->pos,&cp))return fail(p,esc,"malformed \\u escape");
        p->pos+=4;
        if(cp>=0xD800u&&cp<=0xDBFFu){
            // a high surrogate is only half a code point; the low half must
            // follow as its own \u escape or the text is not valid UTF-16.
            uint32_t lo;
            if(p->len-p->pos<2||p->s[p->pos]!='\\'||p->s[p->pos+1]!='u'||
               !hex4(p,p->pos+2,&lo)||lo<0xDC00u||lo>0xDFFFu)
                return fail(p,esc,"high surrogate without a following low surrogate");
            p->pos+=6;
            cp=0x10000u+((cp-0xD800u)<<10)+(lo-0xDC00u);
        }
        else if(cp>=0xDC00u&&cp<=0xDFFFu)return fail(p,esc,"unpaired low surrogate");
        if(!push_utf8(p,cp))return fail(p,esc,"out of memory");
        continue;
    }
    default: return fail(p,esc,"invalid escape");
    }
    if(!sbuf_push(p,&lit,1))return fail(p,esc,"out of memory");
}
}

// strtod would happily swallow "+1", ".5", "0x10", "inf" and "nan", none of
// which JSON allows, and the text is not NUL-terminated. So validate the span
// against the JSON grammar first, then copy it out and let strtod do the
// arithmetic.
static bool parse_number(Parser* p,JsonValue* out){
size_t start=p->pos;
if(p->pos<p->len&&p->s[p->pos]=='-')p->pos++;
if(p->pos>=p->len||!is_digit(p->s[p->pos]))return fail_byte(p,p->pos,"expected a digit in number");
if(p->s[p->pos]=='0'){
    p->pos++;
    if(p->pos<p->len&&is_digit(p->s[p->pos]))return fail(p,start,"leading zero in number");
}
else while(p->pos<p->len&&is_digit(p->s[p->pos]))p->pos++;
if(p->pos<p->len&&p->s[p->pos]=='.'){
    p->pos++;
    if(p->pos>=p->len||!is_digit(p->s[p->pos]))return fail_byte(p,p->pos,"expected a digit after '.'");
    while(p->pos<p->len&&is_digit(p->s[p->pos]))p->pos++;
}
if(p->pos<p->len&&(p->s[p->pos]=='e'||p->s[p->pos]=='E')){
    p->pos++;
    if(p->pos<p->len&&(p->s[p->pos]=='+'||p->s[p->pos]=='-'))p->pos++;
    if(p->pos>=p->len||!is_digit(p->s[p->pos]))return fail_byte(p,p->pos,"expected a digit in exponent");
    while(p->pos<p->len&&is_digit(p->s[p->pos]))p->pos++;
}
p->sbuf_len=0;
if(!sbuf_push(p,p->s+start,p->pos-start)||!sbuf_push(p,"",1))return fail(p,start,"out of memory");
out->type=JSON_NUMBER;
out->as.num=strtod(p->sbuf,NULL);   // out of range folds to +-HUGE_VAL / 0, as elsewhere in the renderer
return true;
}

static bool parse_lit(Parser* p,const char* word,size_t n){
if(p->len-p->pos<n||memcmp(p->s+p->pos,word,n)!=0)return fail_byte(p,p->pos,"invalid literal");
p->pos+=n;
return true;
}

static bool parse_value(Parser* p,JsonValue* out);

static bool parse_container(Parser* p,JsonValue* out,bool is_obj){
if(++p->depth>MAX_DEPTH)return fail(p,p->pos,"nesting too deep");
char close=is_obj?'}':']';
size_t mark=p->stack_len;   // this container's children pile up above the mark
p->pos++;
skip_ws(p);
bool first=true;
for(;;){
    if(p->pos>=p->len)return fail(p,p->pos,is_obj?"unterminated object":"unterminated array");
    if(p->s[p->pos]==close){ p->pos++; break; }
    if(!first){
        if(p->s[p->pos]!=',')return fail_byte(p,p->pos,is_obj?"expected ',' or '}'":"expected ',' or ']'");
        p->pos++;
        skip_ws(p);
        if(p->pos<p->len&&p->s[p->pos]==close)return fail(p,p->pos,"trailing comma");
    }
    first=false;

    const char* key=NULL;
    if(is_obj){
        if(p->pos>=p->len||p->s[p->pos]!='"')return fail_byte(p,p->pos,"expected a quoted object key");
        if(!parse_string_raw(p))return false;
        key=arena_str(p,p->sbuf,p->sbuf_len);
        if(!key)return fail(p,p->pos,"out of memory");
        skip_ws(p);
        if(p->pos>=p->len||p->s[p->pos]!=':')return fail_byte(p,p->pos,"expected ':' after object key");
        p->pos++;
        skip_ws(p);
    }
    JsonValue v;
    if(!parse_value(p,&v))return false;
    v.key=key;                                  // parse_value cleared it
    if(!stack_push(p,&v))return fail(p,p->pos,"out of memory");
    skip_ws(p);
}
size_t n=p->stack_len-mark;
JsonValue* items=NULL;
if(n){
    if(n>SIZE_MAX/sizeof(JsonValue))return fail(p,p->pos,"container too large");
    items=arena_alloc(p->doc,n*sizeof(JsonValue),alignof(JsonValue));
    if(!items)return fail(p,p->pos,"out of memory");
    memcpy(items,p->stack+mark,n*sizeof(JsonValue));
}
p->stack_len=mark;
p->depth--;
out->type=is_obj?JSON_OBJECT:JSON_ARRAY;
out->key=NULL;
out->as.list.items=items;
out->as.list.count=n;
return true;
}

// Expects p->pos already parked on the first byte of the value.
static bool parse_value(Parser* p,JsonValue* out){
*out=(JsonValue){0};
if(p->pos>=p->len)return fail(p,p->pos,"expected a value but hit end of input");
char c=p->s[p->pos];
switch(c){
case '{': return parse_container(p,out,true);
case '[': return parse_container(p,out,false);
case '"':{
    if(!parse_string_raw(p))return false;
    const char* s=arena_str(p,p->sbuf,p->sbuf_len);
    if(!s)return fail(p,p->pos,"out of memory");
    out->type=JSON_STRING;
    out->as.str=s;
    return true;
}
case 't':
    if(!parse_lit(p,"true",4))return false;
    out->type=JSON_BOOL;
    out->as.b=true;
    return true;
case 'f':
    if(!parse_lit(p,"false",5))return false;
    out->type=JSON_BOOL;
    out->as.b=false;
    return true;
case 'n':
    if(!parse_lit(p,"null",4))return false;
    out->type=JSON_NULL;
    return true;
default:
    if(c=='-'||is_digit(c))return parse_number(p,out);
    return fail_byte(p,p->pos,"expected a value");
}
}

// ---- public -----------------------------------------------------------------

JsonDoc* json_parse(const char* text,size_t len,char* errbuf,size_t errbuf_len){
if(errbuf&&errbuf_len)errbuf[0]='\0';
if(!text||len==0){
    if(errbuf&&errbuf_len)snprintf(errbuf,errbuf_len,"empty input");
    return NULL;
}
JsonDoc* doc=calloc(1,sizeof *doc);
if(!doc){
    if(errbuf&&errbuf_len)snprintf(errbuf,errbuf_len,"out of memory");
    return NULL;
}
doc->next_cap=BLOCK_MIN;
Parser p={.s=text,.len=len,.doc=doc,.errbuf=errbuf,.errbuf_len=errbuf_len};
skip_ws(&p);
bool ok=parse_value(&p,&doc->root);
if(ok){
    skip_ws(&p);
    // one document, one root: anything past it means we were handed a stream or
    // a truncated buffer, and reading it as a glTF chunk would be a guess.
    if(p.pos!=len)ok=fail_byte(&p,p.pos,"expected end of input after the root value");
}
free(p.stack);
free(p.sbuf);
if(!ok){ json_free(doc); return NULL; }
return doc;
}

void json_free(JsonDoc* doc){
if(!doc)return;
Block* b=doc->blocks;
while(b){
    Block* next=b->next;
    free(b);
    b=next;
}
free(doc);
}

const JsonValue* json_root(const JsonDoc* doc){ return doc?&doc->root:NULL; }

JsonType json_type(const JsonValue* v){ return v?v->type:JSON_NULL; }

const JsonValue* json_member(const JsonValue* v,const char* key){
if(!v||v->type!=JSON_OBJECT||!key)return NULL;
for(size_t i=0;i<v->as.list.count;i++){
    const JsonValue* m=&v->as.list.items[i];
    if(m->key&&strcmp(m->key,key)==0)return m;   // first wins; glTF has no duplicate keys
}
return NULL;
}

size_t json_count(const JsonValue* v){
if(!v||(v->type!=JSON_ARRAY&&v->type!=JSON_OBJECT))return 0;
return v->as.list.count;
}

const JsonValue* json_at(const JsonValue* v,size_t index){
if(!v||(v->type!=JSON_ARRAY&&v->type!=JSON_OBJECT))return NULL;
if(index>=v->as.list.count)return NULL;
return &v->as.list.items[index];   // members are stored in document order
}

double json_number(const JsonValue* v,double fallback){
return (v&&v->type==JSON_NUMBER)?v->as.num:fallback;
}

bool json_bool(const JsonValue* v,bool fallback){
return (v&&v->type==JSON_BOOL)?v->as.b:fallback;
}

const char* json_string(const JsonValue* v,const char* fallback){
return (v&&v->type==JSON_STRING)?v->as.str:fallback;
}

double json_member_number(const JsonValue* v,const char* key,double fallback){
return json_number(json_member(v,key),fallback);
}

const char* json_member_string(const JsonValue* v,const char* key,const char* fallback){
return json_string(json_member(v,key),fallback);
}
