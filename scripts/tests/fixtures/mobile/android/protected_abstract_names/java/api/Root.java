package api;
import fixture.Trace;
public abstract class Root extends Base {
    protected abstract boolean chooseValue(Press scope,long bits);
    public boolean run(Press scope,long bits) { Trace.hit(1);boolean value=chooseValue(scope,bits);Trace.hit(4);return value; }
}
