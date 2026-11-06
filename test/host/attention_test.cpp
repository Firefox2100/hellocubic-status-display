// Host tests for the notification ("attention") queue and its JSON mapping. Build and run with scripts/host-tests.sh.
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include "attention_core.h"
#include "attention_json.h"
using namespace Attention;
static Item mk(const char* json, uint32_t now, uint32_t seq=1) {
    JsonDocument d; assert(!deserializeJson(d, json)); Item it; fromJson(d.as<JsonVariantConst>(), now, 1800, seq, it); return it;
}
int main() {
    Queue q; uint32_t now = 1000; bool fresh=false;
    // fields + defaults
    Item a = mk(R"({"id":"tests","kind":"progress","title":"TESTS","progress":40,"agent":"claude","project":"api"})", now);
    assert(a.kind==K_PROGRESS && a.progress==40 && a.display==SHOW_QUEUE && !strcmp(a.value,"40%") && a.agent==AGENT_CLAUDE);
    assert(upsert(q,a,now,&fresh)==0 && fresh);
    // update in place: same id, no new interruption
    Item a2 = mk(R"({"id":"tests","kind":"progress","title":"TESTS","progress":70})", now+5);
    assert(upsert(q,a2,now+5,&fresh)==0 && !fresh && q.items[0].progress==70);
    // approval has a 30 min default ttl and outranks everything
    Item ap = mk(R"({"id":"ap","kind":"approval","title":"Bash","body":"Bash: rm -rf build"})", now+10);
    assert(ap.expires==now+10+1800);
    assert(upsert(q,ap,now+10,&fresh)>=0 && fresh);
    int ord[kMax]; int n=ordered(q,ord,now+10); assert(n==2 && q.items[ord[0]].kind==K_APPROVAL);
    // length limits (title<=27) are cut, not rejected
    Item longi = mk(R"({"title":"0123456789012345678901234567890123456789"})", now);
    assert(strlen(longi.title)==27);
    // queue full of approvals rejects a lesser card (409), never evicts an approval
    for (int i=0;i<kMax-2;i++){ char j[96]; snprintf(j,sizeof j,R"({"id":"ap%d","kind":"approval","title":"x"})",i); assert(upsert(q,mk(j,now),now)>=0);}
    assert(count(q)==kMax);
    assert(upsert(q,mk(R"({"id":"info","kind":"info","title":"hi"})",now),now)<0);
    // expiry
    Queue q2; upsert(q2, mk(R"({"id":"s","kind":"success","title":"OK","ttl_s":10})",100),100);
    assert(expire(q2,105)==0 && expire(q2,111)==1 && count(q2)==0);
    // ttl 0 = until cleared
    upsert(q2, mk(R"({"id":"s","kind":"info","title":"OK","ttl_s":0})",100),100); assert(expire(q2,999999)==0 && clear(q2,"s"));
    // icons: "icon" key, "type" alias, unknown/empty -> none, round trip through toJson
    assert(mk(R"({"title":"x","icon":"email"})",now).icon==ICON_EMAIL);
    assert(mk(R"({"title":"x","type":"meeting"})",now).icon==ICON_MEETING);
    assert(mk(R"({"title":"x","icon":"call","type":"email"})",now).icon==ICON_CALL);   // icon wins over type
    assert(mk(R"({"title":"x","icon":"bogus"})",now).icon==ICON_NONE);
    assert(mk(R"({"title":"x"})",now).icon==ICON_NONE);
    for (int i=1;i<ICON_COUNT;i++) assert(iconFrom(iconName((Icon)i))==(Icon)i);
    { Item e = mk(R"({"id":"m","title":"Standup","type":"meeting"})",now); JsonDocument o; toJson(e,now,o.to<JsonObject>()); assert(!strcmp(o["icon"]|"","meeting")); }
    { Item n = mk(R"({"id":"n","title":"plain"})",now); JsonDocument o; toJson(n,now,o.to<JsonObject>()); assert(o["icon"].isNull()); }
    printf("attention ok\n");
}
