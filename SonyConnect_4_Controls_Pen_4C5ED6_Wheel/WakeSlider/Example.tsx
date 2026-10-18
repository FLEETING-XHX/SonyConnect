import WakeSlider from './WakeSlider';
export default function Example() {
  return <div style={{ width: 320 }}><WakeSlider defaultValue={50} wheel wheelStep={1} bars={32} fillColor="#4C5ED6" trackColor="#D9DEEA" onChange={value => console.log(value)}/></div>;
}
