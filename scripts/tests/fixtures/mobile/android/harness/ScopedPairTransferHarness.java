import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ScopedPairTransferHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned scoped pair transfer"); }
    static Object call(int route,int operation,Pair pair) throws Throwable {
        if(route==0)return operation==0?Client.call(pair):operation==1?Client.key(pair):Client.value(pair);
        if(route==1)return operation==0?ScopedPairTransferCaller.call(pair):operation==1?ScopedPairTransferCaller.key(pair):ScopedPairTransferCaller.value(pair);
        String name=operation==0?"call":operation==1?"key":"value";
        if(route==2) {
            try { return Client.class.getMethod(name,Pair.class).invoke(null,pair); }
            catch(InvocationTargetException error) { throw error.getCause(); }
        }
        if(operation==0)return (Object)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(Object.class,Pair.class)).invokeExact(pair);
        return (String)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(String.class,Pair.class)).invokeExact(pair);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        Method factory=Provider.class.getMethod("make",Pair.class);TypeVariable<?>[] formals=factory.getTypeParameters();
        require(formals.length==2 && formals[0].getBounds()[0]==Object.class && formals[1].getBounds()[0]==Object.class);
        Type[] input=((ParameterizedType)factory.getGenericParameterTypes()[0]).getActualTypeArguments();
        Type[] resultSlots=((ParameterizedType)factory.getGenericReturnType()).getActualTypeArguments();
        require(Arrays.equals(resultSlots,formals));
        for(int i=0;i<2;i++)require(((WildcardType)input[i]).getUpperBounds()[0].equals(formals[i]));
        Trace.failAt=mode>=1&&mode<=6?mode:0;Trace.failure=new RuntimeException("owned pair failure");
        int[] first={7},second={11};Object firstInput=mode==8?null:first,secondInput=mode==9?null:second;
        Pair pair=mode==7?null:new Pair<Object,Object>(firstInput,secondInput);
        Object key=mode==12?new Object():mode==15?first:new String("owned key");
        Object value=mode==13?new Object():mode==15?second:new String("owned value");
        Map map=new LinkedHashMap();if(mode!=14)map.put(key,value);
        Trace.result=mode==10?null:mode==11?new Object():map;Trace.key=key;
        Object sentinel=new Object();
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.pair=Trace.first=Trace.second=Trace.after=sentinel;
            int oldFirst=first[0],oldSecond=second[0];Throwable failure=null;Object actual=null;
            try { actual=call(route,operation,pair); } catch(Throwable error) { failure=error; }
            long expected=mode==1?1:mode==4||mode==7?(mode==7?1:14):mode==2?142:mode==5?1425:
                mode==3||mode==11?14253:142536;
            boolean firstDone=mode!=1&&mode!=4&&mode!=7;
            boolean secondDone=firstDone&&mode!=2&&mode!=5;
            require(Trace.events==expected && first[0]==oldFirst+(firstDone&&firstInput!=null?1:0));
            require(second[0]==oldSecond+(secondDone&&secondInput!=null?3:0));
            require(Trace.pair==(mode==1?sentinel:pair));
            require(Trace.first==(firstDone?firstInput:sentinel) && Trace.second==(secondDone?secondInput:sentinel));
            boolean reachedCaller=mode!=1&&mode!=2&&mode!=3&&mode!=4&&mode!=5&&mode!=7&&mode!=11;
            require(Trace.after==(reachedCaller?Trace.result:sentinel));
            if(mode>=1&&mode<=6)require(failure==Trace.failure);
            else if(mode==7||mode==10&&operation!=0)require(failure instanceof NullPointerException);
            else if(mode==11||operation==1&&(mode==12||mode==15)||operation==2&&(mode==13||mode==15))require(failure instanceof ClassCastException);
            else if(mode==14&&operation==1)require(failure instanceof NoSuchElementException);
            else require(failure==null && actual==(operation==0?Trace.result:operation==1?key:mode==14?null:value));
            if(mode!=14)require(map.get(key)==value && map.size()==1);else require(map.isEmpty());
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
