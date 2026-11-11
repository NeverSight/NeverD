package impl;
import api.*;
public final class Left extends Root {
    public Left() {}
    protected void chooseValue(long bits,float scale,Callback<? super Context,Object> block) {
        Trace.capture(this,block,bits,Float.floatToRawIntBits(scale),2);
        block.apply(Trace.context);Trace.step(5);
    }
}
