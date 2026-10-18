'use client';
import React, { useEffect, useRef, useState, type CSSProperties, type ReactNode } from 'react';
import './ElasticSlider.css';

export interface ElasticSliderProps {
  value?: number; defaultValue?: number; startingValue?: number; maxValue?: number;
  isStepped?: boolean; stepSize?: number;
  onChange?: (value: number) => void; onChangeEnd?: (value: number) => void;
  orientation?: 'horizontal' | 'vertical'; leftIcon?: ReactNode; rightIcon?: ReactNode;
  iconPlacement?: 'outside' | 'inside'; valuePosition?: 'top' | 'bubble' | 'inside' | 'none';
  formatValue?: (value: number) => string; ticks?: boolean | number[]; thumb?: boolean;
  fillFrom?: number; thickness?: number; activeThickness?: number; radius?: number;
  restOpacity?: number; growOnHover?: boolean; jumpToClick?: boolean;
  wheel?: boolean; wheelStep?: number; wheelEndDelay?: number; duration?: number;
  trackColor?: string; rangeColor?: string; iconColor?: string;
  contentColor?: string; fillContentColor?: string; thumbColor?: string;
  disabled?: boolean; ariaLabel?: string; className?: string; style?: CSSProperties;
}
const clamp = (v: number, a: number, b: number) => Math.max(a, Math.min(b, v));
const ease: [number,number,number,number] = [0.23, 1, 0.32, 1];
// CSS cubic-bezier is used directly for render transitions; no springs.
export default function ElasticSlider({
  value: external, defaultValue=50, startingValue=0, maxValue=100,
  isStepped=false, stepSize=1, onChange, onChangeEnd, orientation='horizontal',
  leftIcon, rightIcon, iconPlacement='outside', valuePosition='none',
  formatValue, ticks=false, thumb=false, fillFrom, thickness=6, activeThickness=12,
  radius=40, restOpacity=1, growOnHover=true, jumpToClick=true,
  wheel=true, wheelStep, wheelEndDelay=200, duration=220,
  trackColor='#D9DEEA', rangeColor='#4C5ED6', iconColor='#626B7E',
  contentColor='#333B50', fillContentColor='#FFFFFF', thumbColor='#FFFFFF',
  disabled=false, ariaLabel='Slider', className='', style
}: ElasticSliderProps) {
  const min=Math.min(startingValue,maxValue), max=Math.max(startingValue,maxValue);
  const span=Math.max(0,max-min), step=isStepped&&stepSize>0?stepSize:0;
  const [inner,setInner]=useState(defaultValue);
  const value=clamp(external??inner,min,max);
  const latest=useRef(value); latest.current=value;
  const controlled=external!==undefined;
  const commitRef=useRef<(v:number)=>void>(()=>{});
  const endRef=useRef(onChangeEnd); endRef.current=onChangeEnd;
  const rootRef=useRef<HTMLDivElement>(null),hitRef=useRef<HTMLDivElement>(null);
  const dragRef=useRef<{id:number;origin:number;start:number}|null>(null);
  const [dragging,setDragging]=useState(false),[hover,setHover]=useState(false),[focus,setFocus]=useState(false);
  const endTimer=useRef<ReturnType<typeof setTimeout>|null>(null);
  const vertical=orientation==='vertical';
  const pct=span?100*(value-min)/span:0;
  const originPct=span?100*(clamp(fillFrom??min,min,max)-min)/span:0;
  const start=Math.min(pct,originPct), size=Math.abs(pct-originPct);
  const active=dragging||focus||(growOnHover&&hover);
  const visualThickness=active?activeThickness:thickness;
  const snap=(v:number)=>{
    const c=clamp(v,min,max);
    if(!step)return Number(c.toFixed(8));
    const n=clamp(min+Math.round((c-min)/step)*step,min,max);
    return Number(n.toFixed(8));
  };
  const commit=(v:number)=>{
    const next=snap(v);
    if(next===latest.current)return;
    latest.current=next;
    if(!controlled)setInner(next);
    onChange?.(next);
  };
  commitRef.current=commit;
  const stopEndTimer=()=>{if(endTimer.current!==null){clearTimeout(endTimer.current);endTimer.current=null;}};
  const endSoon=()=>{stopEndTimer();endTimer.current=setTimeout(()=>endRef.current?.(latest.current),wheelEndDelay);};
  useEffect(()=>()=>stopEndTimer(),[]);
  // Non-passive listener: wheel over the slider adjusts it; outside areas scroll normally.
  useEffect(()=>{
    const el=hitRef.current;
    if(!el||!wheel||disabled)return;
    let accumulator=0;
    const onWheel=(e:WheelEvent)=>{
      if(e.ctrlKey)return;
      const d=e.deltaY!==0?e.deltaY:e.deltaX;
      if(!d)return;
      const units=e.deltaMode===1?d/3:e.deltaMode===2?d*3:d/100;
      accumulator+=units;
      const ticks=Math.trunc(accumulator);
      if(ticks){
        accumulator-=ticks;
        const inc=wheelStep&&wheelStep>0?wheelStep:step||Math.max(span/100,0.01);
        commitRef.current(latest.current-clamp(ticks,-5,5)*inc);
        endSoon();
      }
      e.preventDefault();
    };
    el.addEventListener('wheel',onWheel,{passive:false});
    return()=>el.removeEventListener('wheel',onWheel);
  },[wheel,disabled,wheelStep,step,span,wheelEndDelay]);
  const location=(e:React.PointerEvent<HTMLDivElement>)=>{
    const box=hitRef.current!.getBoundingClientRect();
    if(vertical)return clamp((box.bottom-e.clientY)/Math.max(box.height,1),0,1);
    return clamp((e.clientX-box.left)/Math.max(box.width,1),0,1);
  };
  const pointerDown=(e:React.PointerEvent<HTMLDivElement>)=>{
    if(disabled||e.button!==0)return;
    const p=location(e);
    dragRef.current={id:e.pointerId,origin:p,start:pct/100};
    e.currentTarget.setPointerCapture(e.pointerId);
    setDragging(true);
    if(jumpToClick)commit(min+p*span);
  };
  const pointerMove=(e:React.PointerEvent<HTMLDivElement>)=>{
    const drag=dragRef.current;
    if(!drag||e.pointerId!==drag.id)return;
    const pos=location(e);
    commit(min+(jumpToClick?pos:clamp(drag.start+pos-drag.origin,0,1))*span);
  };
  const pointerUp=(e:React.PointerEvent<HTMLDivElement>)=>{
    if(!dragRef.current||e.pointerId!==dragRef.current.id)return;
    if(e.currentTarget.hasPointerCapture(e.pointerId))e.currentTarget.releasePointerCapture(e.pointerId);
    dragRef.current=null;setDragging(false);endRef.current?.(latest.current);
  };
  const keyDown=(e:React.KeyboardEvent<HTMLDivElement>)=>{
    if(disabled)return;
    const small=step||Math.max(span/100,0.01),large=Math.max(small,span/10);
    const map:Record<string,number>={ArrowRight:value+small,ArrowUp:value+small,ArrowLeft:value-small,ArrowDown:value-small,PageUp:value+large,PageDown:value-large,Home:min,End:max};
    if(!(e.key in map))return;
    e.preventDefault();commit(map[e.key]);endRef.current?.(latest.current);
  };
  const label=formatValue?formatValue(value):String(Number(value.toFixed(step?String(step).split('.')[1]?.length??0:1)));
  const transitions=duration<=0?'none':`width ${duration}ms cubic-bezier(${ease.join(',')}), height ${duration}ms cubic-bezier(${ease.join(',')}), left ${duration}ms cubic-bezier(${ease.join(',')}), bottom ${duration}ms cubic-bezier(${ease.join(',')})`;
  const tickValues=Array.isArray(ticks)?ticks:ticks?Array.from({length:11},(_,i)=>min+span*i/10):[];
  const hitStyle:CSSProperties={'--es-track':trackColor,'--es-range':rangeColor,'--es-ink':contentColor,'--es-icon':iconColor,'--es-thumb':thumbColor,'--es-radius':`${radius}px`,'--es-size':`${visualThickness}px`,'--es-opacity':restOpacity} as CSSProperties;
  const fillStyle:CSSProperties=vertical?{bottom:`${start}%`,height:`${size}%`,left:0,right:0,transition:dragging?'none':transitions}:{left:`${start}%`,width:`${size}%`,top:0,bottom:0,transition:dragging?'none':transitions};
  return <div ref={rootRef} className={`elastic-slider ${className}`} data-orientation={orientation} data-disabled={disabled||undefined} style={{...hitStyle,...style}}>
    {valuePosition==='top'&&<span className="elastic-slider__value">{label}</span>}
    <div className="elastic-slider__body">
      {iconPlacement==='outside'&&leftIcon&&<span className="elastic-slider__icon">{leftIcon}</span>}
      <div ref={hitRef} className="elastic-slider__hit" role="slider" tabIndex={disabled?-1:0} aria-label={ariaLabel} aria-valuemin={min} aria-valuemax={max} aria-valuenow={value} aria-valuetext={label} aria-orientation={orientation} aria-disabled={disabled||undefined}
        onPointerDown={pointerDown} onPointerMove={pointerMove} onPointerUp={pointerUp} onPointerCancel={pointerUp} onLostPointerCapture={pointerUp} onKeyDown={keyDown}
        onPointerEnter={()=>setHover(true)} onPointerLeave={()=>setHover(false)} onFocus={()=>setFocus(true)} onBlur={()=>setFocus(false)}>
        <div className="elastic-slider__track" style={{transition:duration<=0?'none':`height 160ms ease-out, width 160ms ease-out`}}>
          {tickValues.map((v,i)=><span key={i} className="elastic-slider__tick" style={vertical?{bottom:`${span?(v-min)/span*100:0}%`}:{left:`${span?(v-min)/span*100:0}%`}}/>)}
          <div className="elastic-slider__range" style={fillStyle}/>
          {iconPlacement==='inside'&&<div className="elastic-slider__inside" style={{color:fillContentColor}}><span>{leftIcon}</span><span>{valuePosition==='inside'?label:rightIcon}</span></div>}
        </div>
        {thumb&&<span className="elastic-slider__thumb" style={vertical?{bottom:`${pct}%`,transition:dragging?'none':`bottom ${duration}ms cubic-bezier(${ease.join(',')})`}:{left:`${pct}%`,transition:dragging?'none':`left ${duration}ms cubic-bezier(${ease.join(',')})`}}/>}
        {valuePosition==='bubble'&&(dragging||hover||focus)&&<span className="elastic-slider__bubble" style={vertical?{bottom:`${pct}%`}:{left:`${pct}%`}}>{label}</span>}
      </div>
      {iconPlacement==='outside'&&rightIcon&&<span className="elastic-slider__icon">{rightIcon}</span>}
    </div>
  </div>;
}
