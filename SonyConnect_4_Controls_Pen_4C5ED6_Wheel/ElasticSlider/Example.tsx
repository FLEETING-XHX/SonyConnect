import ElasticSlider from './ElasticSlider';
export default function Example() {
  return <div style={{width:320}}><ElasticSlider defaultValue={50} wheel wheelStep={1} startingValue={0} maxValue={100} thickness={6} activeThickness={12} rangeColor="#4C5ED6" trackColor="#D9DEEA" valuePosition="none" onChange={v=>console.log('volume',v)}/></div>;
}
