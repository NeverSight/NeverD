package impl;
import api.*;
public class Right extends Root {
    public Right() {}
    protected void chooseValue(long bits,float scale,Callback<? super Context,Object> block) {
        Trace.step(7);Trace.capture(this,block,bits,Float.floatToRawIntBits(scale),3);
        block.apply(Trace.context);Trace.step(5);
    }
    protected void chooseValue(long bits,float scale,Token token) {
        Trace.step(7);super.chooseValue(bits,scale,token);
    }
}
