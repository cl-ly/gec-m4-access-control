(function(){
'use strict';

var kind='',timer=null,busy=false;
/* 站点可能挂在子路径下 (例如 nginx 的 /door/), 所有接口一律按当前目录前缀拼相对路径,
   否则 fetch('/api/state') 会打到站点根目录, 拿到 404. */
var BASE=(function(){
  var p=location.pathname||'/';
  if(p.charAt(p.length-1)!=='/')p=p.replace(/[^\/]*$/,'');
  return p;
})();
var KIND_CN={card:'刷卡',password:'密码',remote:'远程',admin:'后台',system:'系统'};
var RES_CLS={OK:'tag-ok',SENT:'tag-warn',DENY:'tag-deny',LOCK:'tag-warn',FAIL:'tag-deny',INFO:''};

function $(id){return document.getElementById(id);}
function esc(s){return String(s==null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');}

function api(method,path,body){
  var opt={method:method,credentials:'same-origin',headers:{}};
  if(body!==undefined){opt.headers['Content-Type']='application/json';opt.body=JSON.stringify(body);}
  return fetch(BASE+path.replace(/^\//,''),opt).then(function(r){
    return r.json().catch(function(){return {};}).then(function(j){
      if(!r.ok){var e=new Error(j.error||('HTTP '+r.status));e.status=r.status;throw e;}
      return j;
    });
  });
}

function toast(msg,bad){
  var t=$('toast');
  t.textContent=msg;
  t.className='toast'+(bad?' toast-bad':'');
  t.hidden=false;
  clearTimeout(toast._t);
  toast._t=setTimeout(function(){t.hidden=true;},4200);
}

function notice(el,title,text){
  el.innerHTML='<div class="notice"><p class="notice-title">'+esc(title)+'</p><p class="muted">'+esc(text)+'</p></div>';
}

function skeleton(el,cols,rows){
  var h='<table><thead><tr>';
  for(var i=0;i<cols.length;i++){h+='<th>'+esc(cols[i])+'</th>';}
  h+='</tr></thead><tbody>';
  for(var r=0;r<rows;r++){
    h+='<tr>';
    for(var c=0;c<cols.length;c++){h+='<td><span class="skel"></span></td>';}
    h+='</tr>';
  }
  el.innerHTML=h+'</tbody></table>';
}

function renderBoard(board){
  var el=$('board-wrap');
  if(!board||!board.length){return notice(el,'当前没有板子连接','板子上电并连上 WiFi 后会出现在这里, 每 10 秒一次心跳。');}
  var h='<table><thead><tr><th>对端</th><th>连接时间</th><th>最后心跳</th><th>状态</th></tr></thead><tbody>';
  board.forEach(function(p){
    h+='<tr><td class="mono">'+esc(p.peer)+'</td><td class="mono">'+esc(p.since)+'</td><td class="mono">'+esc(p.age)+' 秒前</td>'+
       '<td><span class="tag '+(p.online?'tag-ok':'tag-deny')+'">'+(p.online?'在线':'已掉线')+'</span></td></tr>';
  });
  el.innerHTML=h+'</tbody></table>';
}

function renderStats(s){
  if(!s)return;
  var items=[
    ['刷卡通过',s.today_card_ok,'ok'],
    ['刷卡被拒',s.today_card_deny,'deny'],
    ['密码通过',s.today_pwd_ok,'ok'],
    ['密码错误',s.today_pwd_deny,'deny'],
    ['连错锁定',s.today_pwd_lock,'warn'],
    ['远程开门',s.today_remote,'ok']
  ];
  var h='';
  items.forEach(function(it){
    h+='<div class="stat is-'+it[2]+'"><b>'+esc(it[1])+'</b><span>'+esc(it[0])+'</span></div>';
  });
  $('stats').innerHTML=h;
}

function renderEvents(rows){
  var el=$('events-wrap');
  if(!rows||!rows.length){return notice(el,'暂无记录','刷卡、输密码、远程开门都会在这里留下一条带时间的记录。');}
  var h='<table><thead><tr><th>时间</th><th>类型</th><th>结果</th><th>卡 UID</th><th>详情</th><th>来源</th></tr></thead><tbody>';
  rows.forEach(function(e){
    h+='<tr><td class="mono">'+esc(e.ts)+'</td>'+
       '<td>'+esc(KIND_CN[e.kind]||e.kind)+'</td>'+
       '<td><span class="tag '+(RES_CLS[e.result]||'')+'">'+esc(e.result)+'</span></td>'+
       '<td class="mono">'+esc(e.uid||'—')+'</td>'+
       '<td class="wrap">'+esc(e.detail||'')+(e.latency_ms?(' ・ '+e.latency_ms+' ms'):'')+'</td>'+
       '<td class="mono">'+esc(e.ip||'—')+'</td></tr>';
  });
  el.innerHTML=h+'</tbody></table>';
}

function renderCards(rows){
  var el=$('cards-wrap');
  if(!rows||!rows.length){return notice(el,'还没有刷过卡','在板子上刷一次已配对的卡, 它的 UID 会立刻出现在这里。');}
  var h='<table><thead><tr><th>UID</th><th>备注</th><th>启用</th><th>最近出现</th><th>操作</th></tr></thead><tbody>';
  rows.forEach(function(c){
    h+='<tr><td class="mono">'+esc(c.uid)+'</td>'+
       '<td><input type="text" class="card-label" data-uid="'+esc(c.uid)+'" maxlength="40" value="'+esc(c.label||'')+'" placeholder="例如 大门主卡"></td>'+
       '<td><span class="tag '+(c.enabled?'tag-ok':'tag-deny')+'">'+(c.enabled?'已启用':'已停用')+'</span></td>'+
       '<td class="mono">'+esc(c.last_seen||'—')+'</td>'+
       '<td><button class="btn btn-mini" data-act="save" data-uid="'+esc(c.uid)+'">存备注</button> '+
       '<button class="btn btn-mini" data-act="toggle" data-uid="'+esc(c.uid)+'" data-on="'+(c.enabled?'1':'0')+'">'+(c.enabled?'停用':'启用')+'</button> '+
       '<button class="btn btn-mini btn-danger" data-act="del" data-uid="'+esc(c.uid)+'">删除</button></td></tr>';
  });
  el.innerHTML=h+'</tbody></table>';
}

function renderState(d){
  var online=d.online||0;
  var pill=$('board-pill');
  pill.className='pill '+(online?'pill-ok':'pill-deny');
  pill.textContent=online?('门禁板在线 · '+online):'门禁板离线';

  $('fact-online').textContent=online?(online+' 条长连接'):'未连接';
  $('fact-time').textContent=d.time||'–';
  $('fact-total').textContent=(d.stats&&d.stats.total_events!=null)?d.stats.total_events:'–';

  var last=null;
  (d.events||[]).forEach(function(e){
    if(!last&&e.kind==='remote'&&e.result==='OK'){last=e;}
  });
  $('fact-last').textContent=last?last.ts:'–';

  $('door-title').textContent=online?'可以远程开门':'门禁板未连接';
  $('door-hint').textContent=online
    ?'按下按钮后服务器立即下发开锁指令, 板子动作完成会回执 OK, 这里显示真实耗时。'
    :'检查板子电源与 WiFi。板子每 10 秒心跳一次, 超过 90 秒没有数据即判定离线。';
  $('open-btn').disabled=!online;

  renderStats(d.stats);
  renderBoard(d.board);
  renderEvents(d.events);
  renderCards(d.cards);
}

function refresh(){
  if(busy)return;
  busy=true;
  api('GET','/api/state?limit=60').then(function(d){
    renderState(d);
  }).catch(function(e){
    if(e.status===401){return showLogin();}
    $('board-pill').className='pill pill-deny';
    $('board-pill').textContent='服务不可达';
    notice($('board-wrap'),'读取失败',e.message+' ・ 稍后自动重试');
  }).then(function(){busy=false;});
}

function loadEvents(){
  var el=$('events-wrap');
  skeleton(el,['时间','类型','结果','卡 UID','详情','来源'],4);
  api('GET','/api/events?limit=200'+(kind?('&kind='+encodeURIComponent(kind)):'')).then(function(d){
    renderEvents(d.events);
  }).catch(function(e){
    if(e.status===401){return showLogin();}
    notice(el,'读取失败',e.message);
  });
}

function switchView(v){
  ['overview','events','cards','settings'].forEach(function(n){
    $('view-'+n).hidden=(n!==v);
  });
  Array.prototype.forEach.call(document.querySelectorAll('.nav-btn'),function(b){
    b.classList.toggle('is-on',b.getAttribute('data-view')===v);
  });
  if(v==='events')loadEvents();
  if(v==='overview')refresh();
}

function showApp(){
  $('boot').hidden=true;
  $('login-view').hidden=true;
  $('app').hidden=false;
  refresh();
  if(timer)clearInterval(timer);
  timer=setInterval(function(){
    if($('app').hidden)return;
    if($('view-overview').hidden===false)refresh();
  },5000);
}

function showLogin(){
  if(timer){clearInterval(timer);timer=null;}
  $('boot').hidden=true;
  $('app').hidden=true;
  $('login-view').hidden=false;
  var i=$('login-pwd');
  if(i){i.value='';i.focus();}
}

function bind(){
  $('login-form').addEventListener('submit',function(ev){
    ev.preventDefault();
    var err=$('login-error'),btn=$('login-submit');
    err.hidden=true;
    btn.disabled=true;
    api('POST','/api/login',{password:$('login-pwd').value}).then(function(){
      showApp();
    }).catch(function(e){
      err.textContent=e.message;
      err.hidden=false;
    }).then(function(){btn.disabled=false;});
  });

  $('logout').addEventListener('click',function(){
    api('POST','/api/logout',{}).catch(function(){}).then(showLogin);
  });

  Array.prototype.forEach.call(document.querySelectorAll('.nav-btn'),function(b){
    b.addEventListener('click',function(){switchView(b.getAttribute('data-view'));});
  });

  $('event-filters').addEventListener('click',function(ev){
    var b=ev.target.closest('.chip');
    if(!b)return;
    Array.prototype.forEach.call(this.querySelectorAll('.chip'),function(c){c.classList.remove('is-on');});
    b.classList.add('is-on');
    kind=b.getAttribute('data-kind')||'';
    loadEvents();
  });

  $('open-btn').addEventListener('click',function(){
    var btn=this,note=$('open-result');
    btn.disabled=true;
    note.hidden=false;
    note.className='inline-note';
    note.textContent='正在下发开锁指令…';
    api('POST','/api/open',{}).then(function(d){
      note.className='inline-note is-good';
      note.textContent='已下发给 '+d.sent+' 条连接 ・ '+d.time+'。板子动作完成后回执会记入开门记录。';
      toast('开锁指令已下发');
      refresh();
    }).catch(function(e){
      note.className='inline-note is-bad';
      note.textContent=e.message;
      toast(e.message,true);
    }).then(function(){btn.disabled=false;});
  });

  $('cards-wrap').addEventListener('click',function(ev){
    var b=ev.target.closest('button[data-act]');
    if(!b)return;
    var uid=b.getAttribute('data-uid'),act=b.getAttribute('data-act');
    if(act==='save'){
      var inp=this.querySelector('input[data-uid="'+uid+'"]');
      api('POST','/api/cards/update',{uid:uid,label:inp?inp.value:''}).then(function(){
        toast('备注已保存');refresh();
      }).catch(function(e){toast(e.message,true);});
      return;
    }
    if(act==='toggle'){
      api('POST','/api/cards/update',{uid:uid,enabled:b.getAttribute('data-on')!=='1'}).then(function(){
        toast('门卡状态已更新');refresh();
      }).catch(function(e){toast(e.message,true);});
      return;
    }
    if(act==='del'){
      if(!window.confirm('确定从后台删除这张卡 '+uid+' ? 固件里配对的卡不受影响, 这里只是记录。'))return;
      api('POST','/api/cards/delete',{uid:uid}).then(function(){
        toast('已删除');refresh();
      }).catch(function(e){toast(e.message,true);});
    }
  });

  $('pwd-form').addEventListener('submit',function(ev){
    ev.preventDefault();
    var msg=$('pwd-msg');
    var oldv=$('pwd-old').value,newv=$('pwd-new').value,new2=$('pwd-new2').value;
    if(newv!==new2){
      msg.hidden=false;msg.className='inline-note is-bad';msg.textContent='两次输入的新密码不一致';
      return;
    }
    api('POST','/api/password',{old:oldv,new:newv}).then(function(){
      msg.hidden=false;msg.className='inline-note is-good';msg.textContent='密码已更新, 请用新密码重新登录';
      setTimeout(showLogin,1200);
    }).catch(function(e){
      msg.hidden=false;msg.className='inline-note is-bad';msg.textContent=e.message;
    });
  });
}

function boot(){
  bind();
  api('GET','/api/session').then(function(d){
    if(d.authed)showApp();else showLogin();
  }).catch(function(){
    $('boot').textContent='无法连接门禁服务, 请检查服务是否在运行';
    setTimeout(boot,4000);
  });
}

boot();
})();
