const $=id=>document.getElementById(id);
let records=[],presenceRecords=[],selectedPresent=new Set(),selectedCardUID="",editingIndex=null,clockEpoch=0,clockUpdatedAt=0,ws,durationLoaded=false,presenceRequestId=0,firstSocketOpen=true;

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
function renderCards(){
  const query=normalize($("cardSearch").value),body=$("cards"),tableWrap=body.closest(".tableWrap");body.replaceChildren();
  if(selectedCardUID&&!records.some(x=>x.uid===selectedCardUID))selectedCardUID="";
  const visible=records.filter(x=>{
    const initials=[x.firstName,x.middleName].map(v=>Array.from(v||"")[0]||"").join("");
    const date=(x.birthDate||"").split("-"),local=date.length===3?`${date[2]}.${date[1]}.${date[0]}`:"";
    return [x.uid,x.lastName,x.firstName,x.middleName,x.vehicle,x.birthDate,x.address,x.phone,local,initials,(x.lastName||"")+initials].some(v=>normalize(v).includes(query));
  });
  if(selectedCardUID&&!visible.some(x=>x.uid===selectedCardUID))selectedCardUID="";
  for(const x of visible){
    const row=document.createElement("tr");
    row.classList.toggle("attendanceTarget",x.uid===selectedCardUID);
    for(const value of [x.index,x.uid,x.lastName,x.firstName,x.middleName,x.vehicle,x.birthDate,x.address,x.phone,x.relay]){
      const cell=document.createElement("td");cell.textContent=value||"";row.appendChild(cell);
    }
    const actions=document.createElement("td"),select=document.createElement("button"),edit=document.createElement("button"),remove=document.createElement("button");
    select.type=edit.type=remove.type="button";
    select.textContent=x.uid===selectedCardUID?"Обрано":"Обрати";
    select.setAttribute("aria-pressed",String(x.uid===selectedCardUID));
    select.onclick=()=>{
      selectedCardUID=selectedCardUID===x.uid?"":x.uid;
      selectedPresent.clear();
      renderCards();
      renderPresence();
    };
    edit.textContent="Змінити";remove.textContent="Видалити";
    edit.onclick=()=>startEdit(x);remove.onclick=()=>removeCard(x,remove);
    actions.append(select,edit,remove);row.appendChild(actions);body.appendChild(row);
  }
  const rows=Array.from(body.rows);
  tableWrap.style.maxHeight=rows.length>10
    ? `${$("cards").closest("table").tHead.offsetHeight+rows.slice(0,10).reduce((height,row)=>height+row.offsetHeight,0)}px`
    :"";
  $("cardListStatus").textContent=`Показано ${visible.length} із ${records.length} карток`;
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
      selectedCardUID="";
      if(selectedPresent.has(item.uid))selectedPresent.delete(item.uid);
      else if(selectedPresent.size>=10){$("presenceStatus").textContent="Можна вибрати не більше 10 присутніх."}
      else selectedPresent.add(item.uid);
      renderCards();
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
  const requestId=++presenceRequestId;
  try{
    const state=await api("/api/presence");
    if(!Array.isArray(state.cards))throw new Error("Сервер повернув некоректний список присутніх");
    if(requestId!==presenceRequestId)return;
    presenceRecords=state.cards;renderPresence();
  }catch(error){
    if(requestId!==presenceRequestId)return;
    $("presenceStatus").textContent=error instanceof TypeError
      ?"Немає зв’язку з пристроєм; повторна спроба буде автоматично."
      :`Не вдалося завантажити присутність: ${error.message}`;
    console.error(error);
  }
}
function startEdit(x){
  editingIndex=x.index;
  for(const key of ["uid","lastName","firstName","middleName","vehicle","birthDate","address","phone","relay"])$(key).value=x[key]||"";
  $("uid").readOnly=true;$("cardFormTitle").textContent="Редагувати картку";$("saveCard").textContent="Зберегти зміни";$("cancelEdit").hidden=false;
  $("formStatus").textContent=`Редагування UID ${x.uid}`;window.scrollTo({top:document.body.scrollHeight,behavior:"smooth"});
}
function cancelEdit(){
  editingIndex=null;$("uid").readOnly=false;$("cardFormTitle").textContent="Додати картку";$("saveCard").textContent="Додати картку";$("cancelEdit").hidden=true;
}
async function saveCard(){
  const button=$("saveCard"),status=$("formStatus"),data={};
  for(const key of ["uid","lastName","firstName","middleName","vehicle","birthDate","address","phone","relay"])data[key]=$(key).value;
  if(editingIndex!==null)data.index=editingIndex;
  button.disabled=true;status.textContent="Зберігаю...";
  try{const result=await post(editingIndex===null?"/api/add":"/api/update",data);status.textContent=result.msg||"Збережено";if(editingIndex!==null)cancelEdit();else for(const key of ["uid","lastName","firstName","middleName","vehicle","birthDate","address","phone"])$(key).value="";await refreshCards();await refreshPresence()}
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
  const uids=selectedCardUID?[selectedCardUID]:Array.from(selectedPresent);
  if(!uids.length){$("attendanceStatus").textContent="Знайдіть і виберіть картку або виберіть номерну рамку з присутніх.";return}
  const buttons=[$("markArrived"),$("markDeparted")];
  const results=[];
  buttons.forEach(button=>button.disabled=true);
  try{
    for(const uid of uids){
      try{
        const result=await post("/api/attendance",{uid,action});
        results.push(result.msg);
      }catch(error){results.push(`${uid}: ${error.message}`)}
    }
    $("attendanceStatus").textContent=results.join(" · ");
    await refreshPresence();
  }finally{
    buttons.forEach(button=>button.disabled=false);
  }
}
async function refreshStatus(){
  try{const s=await api("/api/status");$("lastUid").textContent=s.uid||"ще не зчитувалась";$("nfcStatus").textContent=s.pn532_1?"підключено":"не знайдено";$("nfc2Status").textContent=s.pn532_2?"підключено":"не знайдено";$("netStatus").textContent=`${s.network} ${s.ip||""}`;if(s.epoch){clockEpoch=s.epoch;clockUpdatedAt=Date.now()}if(!durationLoaded&&Number.isInteger(s.duration)){$("duration").value=s.duration;durationLoaded=true}}
  catch(error){$("connectionState").textContent=`Немає зв’язку: ${error.message}`}
}
function connectSocket(){
  const scheme=location.protocol==="https:"?"wss":"ws";ws=new WebSocket(`${scheme}://${location.host}/ws`);
  ws.onopen=()=>{$("connectionState").textContent="WebSocket підключено";if(firstSocketOpen){firstSocketOpen=false;return}refreshStatus();refreshCards();refreshPresence()};
  ws.onclose=()=>{$("connectionState").textContent="WebSocket перепідключається";setTimeout(connectSocket,2000)};
  ws.onerror=()=>ws.close();
  ws.onmessage=event=>{try{const s=JSON.parse(event.data);if(s.type==="status"){if(s.uid)$("lastUid").textContent=s.uid;$("nfcStatus").textContent=s.pn532_1?"підключено":"не знайдено";$("nfc2Status").textContent=s.pn532_2?"підключено":"не знайдено";$("netStatus").textContent=`${s.network} ${s.ip||""}`;if(s.epoch){clockEpoch=s.epoch;clockUpdatedAt=Date.now()}}else if(["scan","presence","cards"].includes(s.type)){refreshCards();refreshPresence()}}catch(error){console.error("Некоректне WebSocket-повідомлення",error)}};
}
function updateClock(){
  $("kyivClock").textContent=clockEpoch?new Date((clockEpoch+Math.floor((Date.now()-clockUpdatedAt)/1000))*1000).toLocaleString("uk-UA",{timeZone:"Europe/Kyiv"}):"очікується синхронізація NTP";
}
document.addEventListener("DOMContentLoaded",()=>{
  $("searchForm").onsubmit=e=>{e.preventDefault();renderCards();post("/api/search-log",{query:$("cardSearch").value,matches:$("cards").rows.length}).catch(console.error)};
  $("cardSearch").oninput=renderCards;$("clearSearch").onclick=()=>{$("cardSearch").value="";renderCards()};
  $("saveCard").onclick=saveCard;$("cancelEdit").onclick=cancelEdit;
  $("useLastUid").onclick=async()=>{try{const state=await api("/api/status");if(!state.uid)throw new Error("Ще не зчитано жодної картки");$("uid").value=state.uid}catch(error){$("formStatus").textContent=error.message}};
  $("saveDuration").onclick=async()=>{try{alert((await post("/api/settings",{duration:$("duration").value})).msg)}catch(error){alert(error.message)}};
  document.querySelectorAll("[data-pulse]").forEach(button=>button.onclick=async()=>{try{alert((await post("/api/relay",{action:"pulse",mask:button.dataset.pulse,duration:$("duration").value})).msg)}catch(error){alert(error.message)}});
  document.querySelectorAll("[data-stop]").forEach(button=>button.onclick=async()=>{try{alert((await post("/api/relay",{action:"stop",mask:button.dataset.stop})).msg)}catch(error){alert(error.message)}});
  $("markArrived").onclick=()=>markAttendance("arrive");$("markDeparted").onclick=()=>markAttendance("depart");
  refreshCards();refreshPresence();refreshStatus();connectSocket();
  const pollWithoutSocket=callback=>{if(!ws||ws.readyState!==WebSocket.OPEN)callback()};
  setInterval(()=>pollWithoutSocket(refreshCards),10000);setInterval(()=>pollWithoutSocket(refreshPresence),5000);setInterval(()=>pollWithoutSocket(refreshStatus),10000);setInterval(updateClock,1000);
});
