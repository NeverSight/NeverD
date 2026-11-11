import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public class BoundedSeedFactoryHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned bounded seed factory"); }
    static List call(int route,Object context,int index,List values,Object extra,long bits,Fn factory) throws Throwable {
        if(route==0)return Client.call(context,index,values,extra,bits,factory);
        if(route==1)return BoundedSeedFactoryCaller.call(context,index,values,extra,bits,factory);
        if(route==2) {
            try { return (List)Provider.class.getMethod("fill",Object.class,int.class,List.class,Object.class,long.class,Fn.class).invoke(null,context,index,values,extra,bits,factory); }
            catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (List)MethodHandles.publicLookup().findStatic(Provider.class,"fill",MethodType.methodType(List.class,Object.class,int.class,List.class,Object.class,long.class,Fn.class)).invokeExact(context,index,values,extra,bits,factory);
    }
    static void signature() throws Exception {
        Method method=Provider.class.getMethod("fill",Object.class,int.class,List.class,Object.class,long.class,Fn.class);
        TypeVariable<Method>[] formals=method.getTypeParameters();
        require(formals.length==1 && formals[0].getBounds().length==1 && formals[0].getBounds()[0]==Bound.class && Bound.class.isInterface());
        ParameterizedType seed=(ParameterizedType)method.getGenericParameterTypes()[2],result=(ParameterizedType)method.getGenericReturnType();
        require(seed.getRawType()==List.class && result.getRawType()==List.class && seed.getActualTypeArguments()[0].equals(formals[0]) && result.getActualTypeArguments()[0].equals(formals[0]));
        ParameterizedType factory=(ParameterizedType)method.getGenericParameterTypes()[5];
        require(factory.getRawType()==Fn.class);
        WildcardType input=(WildcardType)factory.getActualTypeArguments()[0],output=(WildcardType)factory.getActualTypeArguments()[1];
        require(input.getLowerBounds().length==1 && input.getLowerBounds()[0]==Integer.class && input.getUpperBounds()[0]==Object.class);
        require(output.getLowerBounds().length==0 && output.getUpperBounds().length==1 && output.getUpperBounds()[0].equals(formals[0]));
    }
    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);signature();
        for(int attempt=0;attempt<2;attempt++) {
            Object context=mode==15?null:new Object(),extra=new Object();
            int index=mode==1?Integer.MIN_VALUE:123456789+attempt;
            long bits=mode==1?Long.MIN_VALUE:0x7123456789abcdefL+attempt;
            Item initial=new Item(-0x123456789abcdefL-attempt),item=new Item(bits);
            List seed=new ArrayList();seed.add(mode==14?new Object():initial);
            if(mode==13)seed=Collections.unmodifiableList(seed);
            if(mode==2)seed=null;
            List resultInput=mode==4?null:new ArrayList();
            Trace.context=Trace.extra=Trace.seed=Trace.factory=Trace.item=Trace.observed=Trace.result=Trace.argument=null;
            Trace.resultInput=resultInput;Trace.bits=Trace.observedValue=Trace.events=0;Trace.calls=0;
            int[] failures={0,0,0,0,0,0,0,0,1,3,4,5,6,0,0,0};Trace.failAt=failures[mode];Trace.failure=new IllegalStateException("owned factory failure");
            Fn factory=new Fn() { public Object apply(Object argument) {
                require(argument instanceof Integer && ((Integer)argument).intValue()==index && Trace.bits==bits && Trace.context==context && Trace.extra==extra);
                Trace.calls++;Trace.argument=argument;Trace.hit(2);if(mode==7)throw Trace.failure;
                return mode==6?new Object():mode==5?null:item;
            }};
            if(mode==3)factory=null;
            Throwable caught=null;List actual=null;
            try { actual=call(route,context,index,seed,extra,bits,factory); } catch(Throwable failure) { caught=failure; }
            if(mode==2||mode==3)require(caught instanceof NullPointerException);
            else if(mode==6||mode==14)require(caught instanceof ClassCastException);
            else if(mode==13)require(caught instanceof UnsupportedOperationException);
            else if(mode==7||(mode>=8&&mode<=12))require(caught==Trace.failure);
            else require(caught==null && actual==resultInput && (actual==null||actual!=seed));
            long[] events={123456,123456,123,1,123456,123456,12,12,1,123,1234,12345,123456,123,1234,123456};
            require(Trace.events==events[mode] && Trace.context==context && Trace.extra==extra && Trace.seed==seed && Trace.factory==factory && Trace.bits==bits);
            boolean called=mode!=3&&mode!=8;
            require(Trace.calls==(called?1:0) && (Trace.argument!=null)==called);
            boolean publishedItem=called&&mode!=6&&mode!=7;
            require(Trace.item==(publishedItem&&mode!=5?item:null));
            boolean added=mode==0||mode==1||mode==4||mode==5||mode==10||mode==11||mode==12||mode==14||mode==15;
            if(seed!=null)require(seed.size()==(added?2:1) && (!added||seed.get(1)==(mode==5?null:item)));
            boolean observed=added&&mode!=10&&mode!=14;
            require(Trace.observed==(observed?initial:null) && Trace.observedValue==(observed?initial.bits:0));
            boolean publishedResult=observed&&mode!=11;
            require(Trace.result==(publishedResult?resultInput:null));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
        }
    }
}
