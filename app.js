(() => {
  const $ = (id) => document.getElementById(id);
  const canvas = $('board');
  const ctx = canvas.getContext('2d');
  const wrap = $('boardWrap');
  let state = null, visible = { black: [], highway: [] };
  let viewX = 0, viewY = 0, cellSize = 10;
  let running = false, pending = false, previousFrame = performance.now();
  let visibleRequest = 0, dragging = false, lastPointer = null;
  const directionNames = ['上', '右', '下', '左'];
  const formatter = new Intl.NumberFormat('ja-JP');

  async function api(path, method='GET', body=null) {
    const options = { method, headers: {} };
    if (body !== null) { options.headers['Content-Type'] = 'application/json'; options.body = JSON.stringify(body); }
    const response = await fetch(path, options);
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
    return response.json();
  }
  function fmt(v) { return formatter.format(Number(v || 0)); }
  function setRunning(on) {
    running = on;
    $('runBtn').textContent = on ? 'Ⅱ 一時停止' : '▶ 実行';
    $('runStatus').textContent = on ? '実行中' : '停止中';
    $('runStatus').classList.toggle('active', on);
    previousFrame = performance.now();
  }
  function centerOnAnts() {
    if (!state) return;
    const rect = canvas.getBoundingClientRect();
    const cols = Math.ceil(rect.width / cellSize), rows = Math.ceil(rect.height / cellSize);
    const mx = (state.ants[0].x + state.ants[1].x) / 2;
    const my = (state.ants[0].y + state.ants[1].y) / 2;
    viewX = Math.floor(mx - cols / 2);
    viewY = Math.floor(my - rows / 2);
  }
  function resizeCanvas() {
    const r = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(r.width * dpr));
    canvas.height = Math.max(1, Math.round(r.height * dpr));
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    if (state) { centerOnAnts(); refreshVisible().catch(showError); }
    draw();
  }
  async function refreshVisible() {
    if (!state) return;
    const r = canvas.getBoundingClientRect();
    const w = Math.ceil(r.width / cellSize) + 2, h = Math.ceil(r.height / cellSize) + 2;
    const id = ++visibleRequest;
    const result = await api(`/api/visible?x0=${viewX}&y0=${viewY}&w=${w}&h=${h}`);
    if (id !== visibleRequest) return;
    visible = result;
    draw();
  }
  function showError(error) {
    $('loading').textContent = `C++サーバーに接続できません：${error.message}。READMEの手順でサーバーを起動してください。`;
    $('loading').classList.remove('hidden');
    setRunning(false);
  }
  function applyState(next, recenter=false) {
    state = next;
    $('loading').classList.add('hidden');
    if (recenter) centerOnAnts();
    updateMetrics();
    refreshVisible().catch(showError);
  }
  function viewportQuery() {
    const r = canvas.getBoundingClientRect();
    const w = Math.ceil(r.width / cellSize) + 2;
    const h = Math.ceil(r.height / cellSize) + 2;
    return `x0=${Math.floor(viewX)}&y0=${Math.floor(viewY)}&w=${w}&h=${h}`;
  }
  function applyStepResult(result) {
    // A single server request now returns both simulation metrics and the visible grid.
    state = result.state;
    visible = result.visible;
    $('loading').classList.add('hidden');
    updateMetrics();
    draw();
  }
  function updateMetrics() {
    if (!state) return;
    $('steps').textContent = fmt(state.steps);
    $('visited').textContent = fmt(state.visitedCells);
    $('black').textContent = fmt(state.blackCells);
    $('highwayCells').textContent = fmt(state.highwayCells);
    $('nonHighwayBlack').textContent = fmt(state.nonHighwayBlackCells);
    for (let i=0; i<2; i++) {
      const a = state.ants[i];
      $(`ant${i+1}Pos`).textContent = `位置 (${a.x}, ${a.y})・向き ${directionNames[a.dir]}`;
      const line = $(`ant${i+1}Highway`);
      if (a.highway) {
        line.textContent = `周期開始の推定：${fmt(a.highwayStart)}歩（検出確認：${fmt(a.highwayConfirmedAt)}歩）`;
        line.classList.add('success');
      } else {
        line.textContent = 'ハイウェイ未検出（104歩周期を観測中）';
        line.classList.remove('success');
      }
    }
    const h1=state.ants[0], h2=state.ants[1];
    const hs=$('highwaySummary');
    if(state.bothHighway) {
      hs.textContent = `2匹ともハイウェイを検出。蟻1：開始推定 ${fmt(h1.highwayStart)} 歩、蟻2：開始推定 ${fmt(h2.highwayStart)} 歩。確認に必要だった総歩数：${fmt(Math.max(h1.highwayConfirmedAt,h2.highwayConfirmedAt))} 歩。`;
      hs.className='analysis-result success';
    } else if(h1.highway || h2.highway) {
      const done=[]; if(h1.highway)done.push('蟻1'); if(h2.highway)done.push('蟻2');
      hs.textContent = `${done.join('・')}でハイウェイ候補を確認済み。もう一方の蟻について周期を観測中です。`;
      hs.className='analysis-result warning';
    } else {
      hs.textContent = '検出待ち：各蟻で104歩パターンと移動量の反復を20周期分確認します。';
      hs.className='analysis-result';
    }
    const l=state.loop, ls=$('loopSummary');
    if(l.found) {
      ls.textContent = `既知状態と一致しました。初回：${fmt(l.firstStep)} 歩、再出現：${fmt(l.detectedStep)} 歩、周期：${fmt(l.period)} 歩。`;
      ls.className='analysis-result success';
    } else {
      ls.textContent = 'ループ未検出：相対座標・両方の向き・相対盤面ハッシュを各歩で照合中です。';
      ls.className='analysis-result';
    }
  }
  function draw() {
    const r=canvas.getBoundingClientRect(), w=r.width, h=r.height;
    if(!w || !h) return;
    ctx.clearRect(0,0,w,h); ctx.fillStyle='#fff'; ctx.fillRect(0,0,w,h);
    const cols=Math.ceil(w/cellSize)+1, rows=Math.ceil(h/cellSize)+1;
    // Highway cells are a translucent warm fill below the black cell drawing.
    ctx.fillStyle='#ffe9c7';
    for(const p of visible.highway||[]) {
      const x=(p[0]-viewX)*cellSize, y=(p[1]-viewY)*cellSize;
      ctx.fillRect(x,y,cellSize,cellSize);
    }
    ctx.fillStyle='#171b21';
    for(const p of visible.black||[]) {
      const x=(p[0]-viewX)*cellSize, y=(p[1]-viewY)*cellSize;
      ctx.fillRect(x+0.3,y+0.3,cellSize-0.6,cellSize-0.6);
    }
    ctx.beginPath(); ctx.strokeStyle='#e0e5eb'; ctx.lineWidth=1;
    for(let i=0;i<=cols;i++){const x=Math.round(i*cellSize)+.5;ctx.moveTo(x,0);ctx.lineTo(x,h)}
    for(let i=0;i<=rows;i++){const y=Math.round(i*cellSize)+.5;ctx.moveTo(0,y);ctx.lineTo(w,y)} ctx.stroke();
    if(!state)return;
    for(let i=0;i<2;i++){
      const a=state.ants[i], cx=(a.x-viewX+.5)*cellSize, cy=(a.y-viewY+.5)*cellSize;
      const color=i===0?'#e64d4d':'#347ae0';
      const rad=Math.max(2.7,Math.min(cellSize*.34,7));
      ctx.fillStyle=color;ctx.beginPath();ctx.arc(cx,cy,rad,0,Math.PI*2);ctx.fill();
      ctx.strokeStyle='#fff';ctx.lineWidth=Math.max(1,cellSize*.1);ctx.stroke();
      const vx=[0,1,0,-1][a.dir], vy=[-1,0,1,0][a.dir];
      ctx.beginPath();ctx.moveTo(cx,cy);ctx.lineTo(cx+vx*rad*1.8,cy+vy*rad*1.8);ctx.strokeStyle='#fff';ctx.lineWidth=Math.max(1,cellSize*.13);ctx.stroke();
      ctx.beginPath();ctx.arc(cx,cy,rad+1.5,0,Math.PI*2);ctx.strokeStyle=color;ctx.lineWidth=1;ctx.stroke();
    }
  }
  async function runBatch(n) {
    if(pending)return;
    pending=true;
    try { const next=await api(`/api/step?${viewportQuery()}`,'POST',{count:n}); applyStepResult(next); }
    catch(e){showError(e)} finally {pending=false;}
  }
  $('runBtn').addEventListener('click',()=>setRunning(!running));
  $('step1Btn').addEventListener('click',()=>{setRunning(false);runBatch(1)});
  $('step100Btn').addEventListener('click',()=>{setRunning(false);runBatch(100)});
  $('step1000Btn').addEventListener('click',()=>{setRunning(false);runBatch(1000)});
  $('resetBtn').addEventListener('click',async()=>{
    setRunning(false);
    try { const next=await api('/api/reset','POST',{spacing:Number($('spacing').value)||10,dir1:Number($('dir1').value),dir2:Number($('dir2').value)}); applyState(next,true); }
    catch(e){showError(e)}
  });
  $('speed').addEventListener('input',()=>{$('speedLabel').textContent=fmt($('speed').value)});
  $('cellSize').addEventListener('change',()=>{cellSize=Number($('cellSize').value)||10;centerOnAnts();refreshVisible().catch(showError);draw()});
  let spacingDebounce;
  $('spacing').addEventListener('input',()=>{
    clearTimeout(spacingDebounce);setRunning(false);
    spacingDebounce=setTimeout(async()=>{
      const n=Math.max(1,Math.min(1000000,Number($('spacing').value)||1));
      try {const next=await api('/api/spacing','POST',{spacing:n});applyState(next,true);}
      catch(e){showError(e)}
    },140);
  });
  $('dir1').addEventListener('change',async()=>{setRunning(false);try{applyState(await api('/api/direction','POST',{ant:0,dir:Number($('dir1').value)}));}catch(e){showError(e)}});
  $('dir2').addEventListener('change',async()=>{setRunning(false);try{applyState(await api('/api/direction','POST',{ant:1,dir:Number($('dir2').value)}));}catch(e){showError(e)}});
  wrap.addEventListener('wheel',e=>{
    e.preventDefault();
    const dx=e.shiftKey?(e.deltaX||e.deltaY):e.deltaX, dy=e.shiftKey?0:e.deltaY;
    viewX += Math.round(dx/cellSize); viewY += Math.round(dy/cellSize);
    refreshVisible().catch(showError);
  },{passive:false});
  wrap.addEventListener('pointerdown',e=>{if(e.button!==1)return;e.preventDefault();dragging=true;lastPointer={x:e.clientX,y:e.clientY};wrap.setPointerCapture?.(e.pointerId)});
  wrap.addEventListener('pointermove',e=>{if(!dragging||!lastPointer)return;const dx=e.clientX-lastPointer.x,dy=e.clientY-lastPointer.y;lastPointer={x:e.clientX,y:e.clientY};viewX-=Math.round(dx/cellSize);viewY-=Math.round(dy/cellSize);refreshVisible().catch(showError)});
  wrap.addEventListener('pointerup',()=>{dragging=false;lastPointer=null});wrap.addEventListener('pointercancel',()=>{dragging=false;lastPointer=null});
  window.addEventListener('resize',resizeCanvas);

  async function animation(now) {
    if(running && !pending && state) {
      const elapsed=Math.min(180,Math.max(0,now-previousFrame));
      const count=Math.max(1,Math.min(5000,Math.floor(Number($('speed').value)*elapsed/1000)));
      if(elapsed>=50) { previousFrame=now; pending=true; try{applyStepResult(await api(`/api/step?${viewportQuery()}`,'POST',{count}));}catch(e){showError(e)}finally{pending=false;} }
    } else previousFrame=now;
    requestAnimationFrame(animation);
  }
  async function init(){
    try { state=await api('/api/state'); cellSize=Number($('cellSize').value)||10; resizeCanvas(); applyState(state,true); requestAnimationFrame(animation); }
    catch(e){showError(e)}
  }
  init();
})();
