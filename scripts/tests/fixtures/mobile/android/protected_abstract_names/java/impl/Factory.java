package impl;
import api.*;
public class Factory {
    public static Root create(int kind) { if(kind==0)return new Left();return new Right(); }
    public static boolean local(Root receiver,Press scope,long bits) {
        return ((Middle)receiver).local(scope,bits);
    }
}
