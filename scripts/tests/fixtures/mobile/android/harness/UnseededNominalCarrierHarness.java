import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class UnseededNominalCarrierHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned unseeded nominal carrier"); }
    static Result call(int route,Pair pair) throws Throwable {
        try {
            if(route==0)return Client.call(pair);
            if(route==1)return UnseededNominalCarrierCaller.call(pair);
            if(route==2)return (Result)Provider.class.getMethod("make",Pair.class).invoke(null,pair);
            return (Result)MethodHandles.publicLookup().findStatic(Provider.class,"make",MethodType.methodType(Result.class,Pair.class)).invokeExact(pair);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getMethod("make",Pair.class);TypeVariable<?> formal=method.getTypeParameters()[0];
        require(method.getTypeParameters().length==1&&formal.getBounds()[0]==Object.class&&method.getReturnType()==Result.class);
        Type[] slots=((ParameterizedType)method.getGenericParameterTypes()[0]).getActualTypeArguments();
        ParameterizedType nested=(ParameterizedType)((WildcardType)slots[0]).getUpperBounds()[0];
        require(nested.getRawType()==Carrier.class&&nested.getActualTypeArguments()[0].equals(formal));
        require(((WildcardType)slots[1]).getUpperBounds()[0].equals(formal));
        Object seed=mode==8?null:new Object(),value=mode==12?null:mode==13?seed:new Object(),other=new Object();
        Trace.failure=new RuntimeException("owned nominal failure");Trace.failAt=mode>=1&&mode<=5?mode:0;
        Carrier<Object> owned=mode==14?new Carrier<Object>(value) {public Object read() { reads++;Trace.hit(6);throw Trace.failure; }}:new Carrier<Object>(value);
        Carrier<Object> carrier=mode==7?null:owned;
        Pair pair=mode==6?null:new Pair<Carrier<Object>,Object>(carrier,seed);
        Result first=new Result(value),second=new Result(other);Object sentinel=new Object();
        for(int attempt=0;attempt<2;attempt++) {
            Object reply=mode==9?null:mode==10?new Object():mode==11?second:mode==15&&attempt==1?second:first;
            Trace.events=0;Trace.pair=Trace.carrier=Trace.seed=Trace.extracted=sentinel;Trace.reply=reply;
            int oldReads=owned.reads;Throwable failure=null;Result actual=null;
            try { actual=call(route,pair); } catch(Throwable error) { failure=error; }
            long expected=mode==1||mode==6?1:mode==2?12:mode==3||mode==7?123:mode==5?1235:mode==14?1236:12354;
            require(Trace.events==expected);
            require(Trace.pair==(mode==1?sentinel:pair));
            boolean firstWritten=mode!=1&&mode!=6;
            require(Trace.carrier==(firstWritten?carrier:sentinel));
            boolean seedWritten=firstWritten&&mode!=2;
            require(Trace.seed==(seedWritten?seed:sentinel));
            boolean readCalled=seedWritten&&mode!=3&&mode!=7;
            require(owned.reads==oldReads+(readCalled?1:0));
            boolean extracted=readCalled&&mode!=5&&mode!=14;
            require(Trace.extracted==(extracted?value:sentinel));
            if(mode>=1&&mode<=5||mode==14)require(failure==Trace.failure);
            else if(mode==6||mode==7)require(failure instanceof NullPointerException);
            else if(mode==10)require(failure instanceof ClassCastException);
            else require(failure==null&&actual==reply&&(actual==null||actual.token==(reply==first?value:other)));
            require(owned.value==value);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
