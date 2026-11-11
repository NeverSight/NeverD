import fixture.Core;
public final class DependentCarrierAllocationCaller {
    public static Core<String,Object,java.util.Collection<Object>> first(int capacity,float factor){return new Core<String,Object,java.util.Collection<Object>>(capacity,factor);}
    public static Core<String,Object,java.util.Collection<Object>> second(int capacity,float factor){return new Core<String,Object,java.util.Collection<Object>>(factor,capacity);}
}
