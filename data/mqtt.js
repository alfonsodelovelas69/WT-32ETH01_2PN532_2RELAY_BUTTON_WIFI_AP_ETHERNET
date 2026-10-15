const $=id=>document.getElementById(id);
async function request(url,options={}){
  const response=await fetch(url,{cache:"no-store",credentials:"same-origin",...options});
  const text=await response.text();let data;
  try{data=JSON.parse(text)}catch{throw new Error(text||`HTTP ${response.status}`)}
  if(!response.ok)throw new Error(data.msg||`HTTP ${response.status}`);
  return data;
}
document.addEventListener("DOMContentLoaded",async()=>{
  try{const config=await request("/api/mqtt-config");for(const key of ["host","port","user","topic"])$(key).value=config[key]??""}
  catch(error){$("mqttStatus").textContent=`Не вдалося завантажити налаштування: ${error.message}`}
  $("mqttForm").onsubmit=async event=>{
    event.preventDefault();const button=event.submitter;button.disabled=true;
    const data=Object.fromEntries(["host","port","user","pass","topic"].map(key=>[key,$(key).value]));
    try{$("mqttStatus").textContent=(await request("/mqtt",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(data)})).msg}
    catch(error){$("mqttStatus").textContent=`Не вдалося зберегти: ${error.message}`}
    finally{button.disabled=false}
  };
});
