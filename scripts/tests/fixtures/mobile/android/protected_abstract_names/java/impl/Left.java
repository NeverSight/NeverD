package impl;
import api.*;
import fixture.Trace;
public class Left extends Middle {
    protected final boolean chooseValue(Press scope,long bits) {
        Trace.receiver=this;Trace.scope=scope;Trace.bits=bits;Trace.hit(2);
        scope.touch();Trace.hit(3);boolean value=Trace.output;Trace.result=value;Trace.published=true;return value;
    }
}
