const $=id=>document.getElementById(id);
let records=[],editingIndex=null,clockEpoch=0,clockUpdatedAt=0,ws;

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
async function refreshActivity(){
  try{
    const state=await api("/api/activity"),select=$("recentUids"),selected=select.value;select.replaceChildren();
    for(const item of state.recent){const option=new Option([item.vehicle,item.uid,item.lastName].filter(Boolean).join(" — "),item.uid);select.add(option)}
    if(!state.recent.length)select.add(new Option("Поки немає зчитувань",""));
    if(state.recent.some(x=>x.uid===selected))select.value=selected;
    clockEpoch=state.epoch||0;clockUpdatedAt=Date.now();
    fillEvents("arrivals",state.arrivals);fillEvents("departures",state.departures);
  }catch(error){$("attendanceStatus").textContent=`Не вдалося завантажити активність: ${error.message}`;console.error(error)}
}
function fillEvents(id,items){
  const list=$(id);list.replaceChildren();
  for(const item of items){const li=document.createElement("li");li.textContent=[item.vehicle,item.uid,dateText(item.epoch)].filter(Boolean).join(" — ");list.appendChild(li)}
  if(!items.length){const li=document.createElement("li");li.textContent="Записів поки немає";list.appendChild(li)}
}
function renderRankings(){
  const oldest=records.slice().sort((a,b)=>Number(a.lastReadEpoch||0)-Number(b.lastReadEpoch||0));
  const frequent=records.filter(x=>Number(x.readCount)>0).sort((a,b)=>Number(b.readCount)-Number(a.readCount)||Number(b.lastReadEpoch||0)-Number(a.lastReadEpoch||0));
  for(const [id,items,byCount] of [["oldestReads",oldest,false],["frequentReads",frequent,true]]){
    const list=$(id);list.replaceChildren();
    for(const item of items.slice(0,10)){const li=document.createElement("li");li.textContent=`${item.vehicle?item.vehicle+" — ":""}${item.uid} — ${byCount?`зчитувань: ${item.readCount}`:Number(item.readCount)===0?"ніколи не зчитувалась":dateText(item.lastReadEpoch)}`;list.appendChild(li)}
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
  try{const result=await post(editingIndex===null?"/api/add":"/api/update",data);status.textContent=result.msg||"Збережено";if(editingIndex!==null)cancelEdit();else for(const key of ["uid","lastName","firstName","middleName","vehicle","birthDate"])$(key).value="";await refreshCards()}
  catch(error){status.textContent=`Помилка збереження: ${error.message}`}
  finally{button.disabled=false}
}
async function removeCard(item,button){
  if(!confirm(`Видалити картку ${item.uid}?`))return;
  button.disabled=true;
  try{const result=await post("/api/remove",{index:item.index});$("cardListStatus").textContent=result.msg||"Видалено";await refreshCards()}
  catch(error){$("cardListStatus").textContent=`Не вдалося видалити: ${error.message}`;button.disabled=false}
}
async function markAttendance(action){
  const uid=$("recentUids").value;if(!uid){$("attendanceStatus").textContent="Спочатку зчитайте UID.";return}
  try{const result=await post("/api/attendance",{uid,action});$("attendanceStatus").textContent=result.msg;await refreshActivity()}
  catch(error){$("attendanceStatus").textContent=`Помилка журналу: ${error.message}`}
}
async function refreshStatus(){
  try{const s=await api("/api/status");$("lastUid").textContent=s.uid||"ще не зчитувалась";$("nfcStatus").textContent=s.pn532?"підключено":"не знайдено";$("netStatus").textContent=`${s.network} ${s.ip||""}`}
  catch(error){$("connectionState").textContent=`Немає зв’язку: ${error.message}`}
}
function connectSocket(){
  const scheme=location.protocol==="https:"?"wss":"ws";ws=new WebSocket(`${scheme}://${location.host}/ws`);
  ws.onopen=()=>{$("connectionState").textContent="WebSocket підключено";refreshStatus();refreshActivity()};
  ws.onclose=()=>{$("connectionState").textContent="WebSocket перепідключається";setTimeout(connectSocket,2000)};
  ws.onerror=()=>ws.close();
  ws.onmessage=event=>{try{const s=JSON.parse(event.data);if(s.type==="status"){if(s.uid)$("lastUid").textContent=s.uid;$("nfcStatus").textContent=s.pn532?"підключено":"не знайдено";$("netStatus").textContent=`${s.network} ${s.ip||""}`;if(s.epoch){clockEpoch=s.epoch;clockUpdatedAt=Date.now()}}else if(s.type==="scan"){refreshActivity();refreshCards()}}catch(error){console.error("Некоректне WebSocket-повідомлення",error)}};
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
  refreshCards();refreshActivity();refreshStatus();connectSocket();
  setInterval(refreshCards,10000);setInterval(refreshActivity,15000);setInterval(refreshStatus,10000);setInterval(updateClock,1000);
});
