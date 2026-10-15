const $=id=>document.getElementById(id);
async function request(url,options={}){
  const response=await fetch(url,{cache:"no-store",credentials:"same-origin",...options});
  const text=await response.text();
  let data;try{data=JSON.parse(text)}catch{throw new Error(text||`HTTP ${response.status}`)}
  if(!response.ok)throw new Error(data.msg||data.error||`HTTP ${response.status}`);
  return data;
}
async function scanNetworks(){
  const button=$("scanNetworks"),status=$("scanStatus"),select=$("networks");
  button.disabled=true;status.textContent="Шукаю мережі...";
  try{
    let result=await request("/wifi/scan?start=1");
    while(result.scanning){await new Promise(resolve=>setTimeout(resolve,700));result=await request("/wifi/scan")}
    if(result.error)throw new Error(result.error);
    select.replaceChildren(new Option("Оберіть мережу",""));
    for(const network of result.networks){const option=new Option(`${network.ssid} (${network.rssi} dBm)${network.open?" — відкрита":" — пароль"}`,network.ssid);select.add(option)}
    status.textContent=`Знайдено мереж: ${result.networks.length}`;
  }catch(error){status.textContent=`Помилка пошуку: ${error.message}`}
  finally{button.disabled=false}
}
document.addEventListener("DOMContentLoaded",()=>{
  $("scanNetworks").onclick=scanNetworks;
  $("networks").onchange=()=>{if($("networks").value)$("ssid").value=$("networks").value};
  $("wifiForm").onsubmit=async event=>{
    event.preventDefault();const button=event.submitter;button.disabled=true;
    try{const result=await request("/wifi",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({ssid:$("ssid").value,pass:$("pass").value})});$("wifiSaveStatus").textContent=result.msg}
    catch(error){$("wifiSaveStatus").textContent=`Не вдалося зберегти: ${error.message}`}
    finally{button.disabled=false}
  };
  scanNetworks();
});
