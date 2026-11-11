package impl;
import api.*;
public abstract class Middle extends Root {
    public boolean local(Press scope,long bits) { return this.chooseValue(scope,bits); }
}
