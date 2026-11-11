package api;
public abstract class Root extends Base {
    public Root() {}
    protected abstract void chooseValue(long bits,float scale,Callback<? super Context,Object> block);
    protected void chooseValue(long bits,float scale,Token token) {
        Trace.capture(this,token,bits,Float.floatToRawIntBits(scale),1);Trace.step(5);
    }
    public final void callback(long bits,float scale,Callback<? super Context,Object> block) {
        Trace.step(1);chooseValue(bits,scale,block);Trace.step(6);
    }
    public final void tagged(long bits,float scale,Token token) {
        Trace.step(1);chooseValue(bits,scale,token);Trace.step(6);
    }
}
