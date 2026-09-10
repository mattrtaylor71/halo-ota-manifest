#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Volatile evidence owned by the existing main-task OTA/report path only.
// Seven fixed free/largest pairs, no NVS, task, failure-path allocation or log.
namespace ota_heap {
enum Point:uint8_t {BeforeDma,AfterDma,AfterMqtt,AfterSenseManifest,
                    AfterLcdManifest,AfterLcdScope,BeforeApply,Count};
struct Trace {uint32_t pairs[Count][2];uint8_t mask;bool lcd_proxy_invoked;};
static Trace trace{};
static_assert(sizeof(Trace)==60,"fixed heap observation cost");
inline void reset(){trace=Trace{};}
inline void sample(Point p){
  if(p>=Count)return;
  trace.pairs[p][0]=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  trace.pairs[p][1]=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  trace.mask|=uint8_t(1u<<p);
}
inline void proxy_invoked(){trace.lcd_proxy_invoked=true;}
inline void report(JsonObject& payload){
  if(!trace.mask)return;
  payload["ota_heap_schema"]=1;payload["ota_heap_mask"]=trace.mask;
  payload["ota_lcd_proxy_invoked"]=trace.lcd_proxy_invoked?1:0;
  JsonArray pairs=payload["ota_heap_pairs"].to<JsonArray>();
  for(uint8_t i=0;i<Count;++i){pairs.add(trace.pairs[i][0]);pairs.add(trace.pairs[i][1]);}
}
} // namespace ota_heap
