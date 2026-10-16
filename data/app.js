const $=id=>document.getElementById(id);
let records=[],presenceRecords=[],selectedPresent=new Set(),selectedRecentUid="",editingIndex=null,clockEpoch=0,clockUpdatedAt=0,ws,durationLoaded=false;

async function api(url,options={}){
  const response=await fetch(url,{cache:"no-store",credentials:"same-origin",...options});
  const text=await response.text();
  let data;
  try{data=JSON.parse(text)}catch{throw new Error(`Некоректна відповідь сервера (HTTP ${response.status})`)}
  if(!response.ok)throw new Error(data.msg||data.error||`HTTP ${response.status}`);
  return data;
}
const post=(url,data)=>api(url,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(data)});
const normalize=value=>String(value||"").toLocaleLowerCase().replace(/[.\s-]+/g,"");
function detectPlateRegion(plate){
  const value=String(plate||"").trim().toUpperCase(),compact=value.replace(/[\s.-]+/g,"");
  const matches=[];
  const add=(code,name,priority=10)=>matches.push({code,name,priority});
  if(/^[A-Z]{2}\d{4}[A-Z]{2}$/.test(compact))add("UA","Україна");
  if(/^[A-Z]{2,3}\s+\d{4,5}$/.test(value))add("PL","Польща");
  if(/^[A-Z]{2}\d{2}[A-Z]{3}$/.test(compact))add("GB","Велика Британія");
  if(/^[A-Z]{1,3}-[A-Z]{1,2}\s?\d{1,4}$/.test(value))add("DE","Німеччина");
  if(/^[A-Z]{2}-\d{3}-[A-Z]{2}$/.test(value))add("FR","Франція");
  if(/^[A-Z]{2}\s+\d{3}\s+[A-Z]{2}$/.test(value))add("IT","Італія");
  if(/^\d{4}\s*[BCDFGHJKLMNPRSTVWXYZ]{3}$/.test(value))add("ES","Іспанія");

  const floridaColoradoMatch=/^([A-Z])[A-Z]{3}\d{2}$/.exec(compact);
  if(floridaColoradoMatch){
    const code=floridaColoradoMatch[1]<="N"?"FL":"CO";
    add(code,code==="FL"?"Флорида, США":"Колорадо, США",24);
  }
  const usStates=[
    ["CA","Каліфорнія",/^\d[A-Z]{3}\d{3}$/,20],
    ["NY","Нью-Йорк",/^[G-K][A-Z]{2}\d{4}$/,22],
    ["PA","Пенсильванія",/^[DEFGHJKL][A-Z]{2}\d{4}$/,20],
    ["TX","Техас",/^[A-Z]{2}\d[A-Z]\d{3}$/,20],
    ["FL","Флорида",/^[A-N][A-Z]{3}\d{3}$/,20],
    ["IL","Іллінойс",/^[A-Z]\d{6}$/,20],
    ["NJ","Нью-Джерсі",/^[A-Z]\d{2}[A-Z]{3}$/,20],
    ["OH","Огайо",/^[G-H][A-Z]{2}\d{4}$/,23],
    ["VA","Вірджинія",/^[U-Z][A-Z]{2}\d{4}$/,20],
    ["AZ","Аризона",/^[A-Z]{3}\d{2}[A-Z]{2}$/,20],
    ["ID","Айдахо",/^[1-9][A-Z]\d{5}$/,20],
    ["WY","Вайомінг",/^(?:[1-9]|1\d|2[0-3])-\d{5}$/,20],
    ["MT","Монтана",/^(?:[1-9]|[1-5]\d|56)-\d{5}[A-Z]$/,20],
    ["MD","Меріленд",/^1[A-Z]{2}\d{4}$/,20],
    ["MA","Массачусетс",/^\d{3}[A-Z]{2}\d$/,20],
    ["RI","Род-Айленд",/^[A-Z]{2}\d{3}$/,20]
  ];
  for(const [code,name,pattern,priority] of usStates){
    const input=code==="WY"||code==="MT"?value:compact;
    if(pattern.test(input))add(code,`${name}, США`,priority);
  }
  if(!matches.length)return null;
  matches.sort((a,b)=>b.priority-a.priority);
  return matches.length>1&&matches[0].priority===matches[1].priority?null:matches[0];
}
function dateText(epoch){return epoch?new Date(epoch*1000).toLocaleString("uk-UA",{timeZone:"Europe/Kyiv"}):"час не синхронізовано"}
function renderCards(){
  const query=normalize($("cardSearch").value),body=$("cards");body.replaceChildren();
  const visible=records.filter(x=>{
    const initials=[x.firstName,x.middleName].map(v=>Array.from(v||"")[0]||"").join("");
    const date=(x.birthDate||"").split("-"),local=date.length===3?`${date[2]}.${date[1]}.${date[0]}`:"";
    return [x.uid,x.lastName,x.firstName,x.middleName,x.vehicle,x.birthDate,local,initials,(x.lastName||"")+initials].some(v=>normalize(v).includes(query));
  });
  for(const x of visible){
    const row=document.createElement("tr");
    for(const value of [x.index,x.uid,x.lastName,x.firstName,x.middleName,x.vehicle,x.birthDate,x.relay]){
      const cell=document.createElement("td");cell.textContent=value||"";row.appendChild(cell);
    }
    const actions=document.createElement("td"),edit=document.createElement("button"),remove=document.createElement("button");
    edit.type=remove.type="button";edit.textContent="Змінити";remove.textContent="Видалити";
    edit.onclick=()=>startEdit(x);remove.onclick=()=>removeCard(x,remove);
    actions.append(edit,remove);row.appendChild(actions);body.appendChild(row);
  }
  $("cardListStatus").textContent=`Показано ${visible.length} із ${records.length} карток`;
  renderRankings();
}
async function refreshCards(){
  try{records=await api("/api/list");if(!Array.isArray(records))throw new Error("Сервер повернув не список");renderCards()}
  catch(error){$("cardListStatus").textContent=`Не вдалося завантажити список: ${error.message}`;console.error(error)}
}
function renderPresence(){
  const present=presenceRecords.filter(item=>item.present),tiles=$("presentVehicles");
  tiles.replaceChildren();
  $("presentCount").textContent=String(present.length);
  const presentUids=new Set(present.map(item=>item.uid));
  for(const uid of selectedPresent)if(!presentUids.has(uid))selectedPresent.delete(uid);
  const slotCount=Math.max(6,present.length);
  for(let i=0;i<slotCount;i++){
    const item=present[i];
    if(!item){
      const emptyTile=document.createElement("div");
      emptyTile.className="presenceTile presenceTileEmpty";
      emptyTile.textContent="Вільне місце";
      emptyTile.setAttribute("aria-hidden","true");
      tiles.appendChild(emptyTile);
      continue;
    }
    const tile=document.createElement("button"),name=[item.lastName,item.firstName,item.middleName].filter(Boolean).join(" ");
    const region=detectPlateRegion(item.vehicle);
    tile.type="button";tile.className="presenceTile";tile.textContent=item.vehicle||item.uid;
    if(region)tile.dataset.country=region.code;
    else tile.classList.add("presenceTileNoCountry");
    tile.title=[region&&region.name,item.vehicle,item.uid,name].filter(Boolean).join(" — ");
    tile.setAttribute("aria-label",[region&&region.name,item.vehicle,name].filter(Boolean).join(" — "));
    tile.setAttribute("aria-pressed",String(selectedPresent.has(item.uid)));
    if(selectedPresent.has(item.uid))tile.classList.add("selected");
    tile.onclick=()=>{
      if(selectedPresent.has(item.uid))selectedPresent.delete(item.uid);
      else if(selectedPresent.size>=10){$("presenceStatus").textContent="Можна вибрати не більше 10 присутніх."}
      else selectedPresent.add(item.uid);
      renderPresence();
    };
    tiles.appendChild(tile);
  }
  const visitors=$("selectedVisitors");visitors.replaceChildren();
  const selected=present.filter(item=>selectedPresent.has(item.uid));
  for(const item of selected){
    const tile=document.createElement("article"),title=document.createElement("strong"),details=document.createElement("div");
    title.textContent=item.vehicle||item.uid;
    const region=detectPlateRegion(item.vehicle);
    details.textContent=[region&&region.name,item.uid,[item.lastName,item.firstName,item.middleName].filter(Boolean).join(" ")].filter(Boolean).join(" — ");
    tile.className="visitorTile";tile.append(title,details);visitors.appendChild(tile);
  }
  if(!selected.length)visitors.textContent="Виберіть плитки присутніх автомобілів (до 10).";
  const absentCount=Math.max(0,presenceRecords.length-present.length);
  $("presenceStatus").textContent=present.length
    ? `Зареєстровано карток: ${presenceRecords.length}; присутніх: ${present.length}; відсутніх: ${absentCount}; вибрано: ${selected.length} із 10. Стан присутності зберігається лише до перезавантаження пристрою.`
    : `Наразі немає присутніх зареєстрованих карток. Зареєстровано: ${presenceRecords.length}; відсутніх: ${absentCount}; вибрано: ${selected.length} із 10. Стан присутності зберігається лише до перезавантаження пристрою.`;
}
async function refreshPresence(){
  try{
    const state=await api("/api/presence");
    if(!Array.isArray(state.cards))throw new Error("Сервер повернув некоректний список присутніх");
    presenceRecords=state.cards;renderPresence();
  }catch(error){
    $("presenceStatus").textContent=error instanceof TypeError
      ?"Немає зв’язку з пристроєм; повторна спроба буде автоматично."
      :`Не вдалося завантажити присутність: ${error.message}`;
    console.error(error);
  }
}
async function refreshActivity(){
  try{
    const state=await api("/api/activity");
    renderRecentReads(state.recent);
    clockEpoch=state.epoch||0;clockUpdatedAt=Date.now();
    fillEvents("arrivals",state.arrivals);fillEvents("departures",state.departures);
  }catch(error){$("attendanceStatus").textContent=`Не вдалося завантажити активність: ${error.message}`;console.error(error)}
}
function renderRecentReads(items){
  const grid=$("recentReads");grid.replaceChildren();
  if(!items.some(item=>item.uid===selectedRecentUid))selectedRecentUid=items[0]?.uid||"";
  for(const item of items){
    const tile=document.createElement("button"),plate=document.createElement("strong"),details=document.createElement("span");
    const region=detectPlateRegion(item.vehicle),name=[item.lastName].filter(Boolean).join(" ");
    tile.type="button";tile.className="activityPlate";tile.classList.toggle("activityPlateSelected",item.uid===selectedRecentUid);
    if(region)tile.dataset.region=region.code;
    else tile.classList.add("activityPlateNoRegion");
    plate.textContent=item.vehicle||item.uid;
    details.textContent=[item.uid,name,item.epoch?dateText(item.epoch):"ще не зчитувалась"].filter(Boolean).join(" — ");
    tile.append(plate,details);
    tile.setAttribute("aria-pressed",String(item.uid===selectedRecentUid));
    tile.setAttribute("aria-label",[region&&region.name,item.vehicle,item.uid,name].filter(Boolean).join(" — "));
    tile.onclick=()=>{selectedRecentUid=item.uid;renderRecentReads(items)};
    grid.appendChild(tile);
  }
  if(!items.length){const empty=document.createElement("p");empty.className="activityEmpty";empty.textContent="Поки немає зчитувань.";grid.appendChild(empty)}
}
function fillEvents(id,items){
  const list=$(id);list.replaceChildren();
  list.className="activityHistory";
  for(const item of items){
    const entry=document.createElement("li"),plate=document.createElement("strong"),details=document.createElement("span"),region=detectPlateRegion(item.vehicle);
    entry.className="activityHistoryEntry";
    plate.className="activityHistoryPlate";
    if(region)plate.dataset.region=region.code;
    else plate.classList.add("activityPlateNoRegion");
    plate.textContent=item.vehicle||item.uid;
    details.textContent=[item.uid,dateText(item.epoch)].filter(Boolean).join(" — ");
    entry.append(plate,details);list.appendChild(entry);
  }
  if(!items.length){const li=document.createElement("li");li.className="activityHistoryEmpty";li.textContent="Записів поки немає";list.appendChild(li)}
}
function renderRankings(){
  const oldest=records.slice().sort((a,b)=>Number(a.lastReadEpoch||0)-Number(b.lastReadEpoch||0));
  const frequent=records.filter(x=>Number(x.readCount)>0).sort((a,b)=>Number(b.readCount)-Number(a.readCount)||Number(b.lastReadEpoch||0)-Number(a.lastReadEpoch||0));
  for(const [id,items,byCount] of [["oldestReads",oldest,false],["frequentReads",frequent,true]]){
    const list=$(id);list.replaceChildren();list.className="activityHistory";
    items.slice(0,10).forEach((item,index)=>{
      const entry=document.createElement("li"),plate=document.createElement("strong"),details=document.createElement("span"),region=detectPlateRegion(item.vehicle);
      entry.className="activityHistoryEntry";
      plate.className="activityHistoryPlate";
      if(region)plate.dataset.region=region.code;
      else plate.classList.add("activityPlateNoRegion");
      plate.textContent=item.vehicle||item.uid;
      details.textContent=`№${index+1} — ${item.uid} — ${byCount?`зчитувань: ${item.readCount}`:Number(item.readCount)===0?"ніколи не зчитувалась":dateText(item.lastReadEpoch)}`;
      entry.append(plate,details);list.appendChild(entry);
    });
  }
}
function startEdit(x){
  editingIndex=x.index;
  for(const key of ["uid","lastName","firstName","middleName","vehicle","birthDate","relay"])$(key).value=x[key]||"";
  $("uid").readOnly=true;$("cardFormTitle").textContent="Редагувати картку";$("saveCard").textContent="Зберегти зміни";$("cancelEdit").hidden=false;
  $("formStatus").textContent=`Редагування UID ${x.uid}`;window.scrollTo({top:document.body.scrollHeight,behavior:"smooth"});
}
function cancelEdit(){
  editingIndex=null;$("uid").readOnly=false;$("cardFormTitle").textContent="Додати картку";$("saveCard").textContent="Додати картку";$("cancelEdit").hidden=true;
}
async function saveCard(){
  const button=$("saveCard"),status=$("formStatus"),data={};
  for(const key of ["uid","lastName","firstName","middleName","vehicle","birthDate","relay"])data[key]=$(key).value;
  if(editingIndex!==null)data.index=editingIndex;
  button.disabled=true;status.textContent="Зберігаю...";
  try{const result=await post(editingIndex===null?"/api/add":"/api/update",data);status.textContent=result.msg||"Збережено";if(editingIndex!==null)cancelEdit();else for(const key of ["uid","lastName","firstName","middleName","vehicle","birthDate"])$(key).value="";await refreshCards();await refreshPresence()}
  catch(error){status.textContent=`Помилка збереження: ${error.message}`}
  finally{button.disabled=false}
}
async function removeCard(item,button){
  if(!confirm(`Видалити картку ${item.uid}?`))return;
  button.disabled=true;
  try{const result=await post("/api/remove",{index:item.index});$("cardListStatus").textContent=result.msg||"Видалено";await refreshCards();await refreshPresence()}
  catch(error){$("cardListStatus").textContent=`Не вдалося видалити: ${error.message}`;button.disabled=false}
}
async function markAttendance(action){
  const uid=selectedRecentUid;if(!uid){$("attendanceStatus").textContent="Спочатку зчитайте UID.";return}
  try{const result=await post("/api/attendance",{uid,action});$("attendanceStatus").textContent=result.msg;await refreshActivity();await refreshPresence()}
  catch(error){$("attendanceStatus").textContent=`Помилка журналу: ${error.message}`}
}
async function refreshStatus(){
  try{const s=await api("/api/status");$("lastUid").textContent=s.uid||"ще не зчитувалась";$("nfcStatus").textContent=s.pn532_1?"підключено":"не знайдено";$("nfc2Status").textContent=s.pn532_2?"підключено":"не знайдено";$("netStatus").textContent=`${s.network} ${s.ip||""}`;if(!durationLoaded&&Number.isInteger(s.duration)){$("duration").value=s.duration;durationLoaded=true}}
  catch(error){$("connectionState").textContent=`Немає зв’язку: ${error.message}`}
}
function connectSocket(){
  const scheme=location.protocol==="https:"?"wss":"ws";ws=new WebSocket(`${scheme}://${location.host}/ws`);
  ws.onopen=()=>{$("connectionState").textContent="WebSocket підключено";refreshStatus();refreshActivity();refreshPresence()};
  ws.onclose=()=>{$("connectionState").textContent="WebSocket перепідключається";setTimeout(connectSocket,2000)};
  ws.onerror=()=>ws.close();
  ws.onmessage=event=>{try{const s=JSON.parse(event.data);if(s.type==="status"){if(s.uid)$("lastUid").textContent=s.uid;$("nfcStatus").textContent=s.pn532_1?"підключено":"не знайдено";$("nfc2Status").textContent=s.pn532_2?"підключено":"не знайдено";$("netStatus").textContent=`${s.network} ${s.ip||""}`;if(s.epoch){clockEpoch=s.epoch;clockUpdatedAt=Date.now()}}else if(["scan","presence","cards"].includes(s.type)){refreshActivity();refreshCards();refreshPresence()}}catch(error){console.error("Некоректне WebSocket-повідомлення",error)}};
}
function updateClock(){
  $("kyivClock").textContent=clockEpoch?new Date((clockEpoch+Math.floor((Date.now()-clockUpdatedAt)/1000))*1000).toLocaleString("uk-UA",{timeZone:"Europe/Kyiv"}):"очікується синхронізація NTP";
}
document.addEventListener("DOMContentLoaded",()=>{
  $("searchForm").onsubmit=e=>{e.preventDefault();renderCards();post("/api/search-log",{query:$("cardSearch").value,matches:$("cards").rows.length}).catch(console.error)};
  $("cardSearch").oninput=renderCards;$("clearSearch").onclick=()=>{$("cardSearch").value="";renderCards()};
  $("saveCard").onclick=saveCard;$("cancelEdit").onclick=cancelEdit;
  $("useLastUid").onclick=async()=>{try{const state=await api("/api/status");if(!state.uid)throw new Error("Ще не зчитано жодної картки");$("uid").value=state.uid}catch(error){$("formStatus").textContent=error.message}};
  $("markArrived").onclick=()=>markAttendance("arrive");$("markDeparted").onclick=()=>markAttendance("depart");
  $("saveDuration").onclick=async()=>{try{alert((await post("/api/settings",{duration:$("duration").value})).msg)}catch(error){alert(error.message)}};
  document.querySelectorAll("[data-pulse]").forEach(button=>button.onclick=async()=>{try{alert((await post("/api/relay",{action:"pulse",mask:button.dataset.pulse,duration:$("duration").value})).msg)}catch(error){alert(error.message)}});
  document.querySelectorAll("[data-stop]").forEach(button=>button.onclick=async()=>{try{alert((await post("/api/relay",{action:"stop",mask:button.dataset.stop})).msg)}catch(error){alert(error.message)}});
  refreshCards();refreshActivity();refreshPresence();refreshStatus();connectSocket();
  setInterval(refreshCards,10000);setInterval(refreshActivity,15000);setInterval(refreshPresence,5000);setInterval(refreshStatus,10000);setInterval(updateClock,1000);
});
