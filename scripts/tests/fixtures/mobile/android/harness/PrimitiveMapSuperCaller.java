import fixture.Core;
public final class PrimitiveMapSuperCaller {
    public static Core<String,Object> first(int capacity,float factor){return new Core<String,Object>(capacity,factor);}
    public static Core<String,Object> second(int capacity,float factor){return new Core<String,Object>(factor,capacity);}
}
