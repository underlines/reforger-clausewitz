import { manifest, situation } from "./agent/fixtures/hq_fixture.mjs";
const B="http://127.0.0.1:8788";
const post=async b=>(await fetch(B+"/orders",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(b)})).json();
let id=900;
async function hq(text,lang){const msg={id:++id,player:"Alice",role:"soldier",squad:"US_Squad1",grid:"009-009",pos:[900,950],text,ageSec:1,...(lang?{lang}:{})};
 for(let i=0;i<90;i++){const j=await post({...situation,v:4,faction:"US",manifest:manifest.hash,hq:[msg]});if(j.hqReplies?.length)return j.hqReplies[0];await new Promise(r=>setTimeout(r,1000));}return null;}
console.log("1", JSON.stringify(await hq("please respond in japanese from now on. what is our status?","en")));
console.log("2", JSON.stringify(await hq("wie ist die lage?","de")));
console.log("3", JSON.stringify(await hq("antworte bitte wieder in der standardsprache","de")));
