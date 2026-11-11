import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class RecursiveRangeResultHarness {
    static int comparisons,lowReads,highReads;
    static Comparable low,high,seed;
    static RuntimeException failure;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned recursive range result");}
    static final class Value implements Comparable<Value> {
        final long number;Value(long number){this.number=number;}
        public int compareTo(Value other){comparisons++;Trace.hit(comparisons==1?4:8);return Long.compare(number,other.number);}
    }
    static final class Band implements Range {
        public Comparable lower(){lowReads++;Trace.hit(2);return low;}
        public Comparable upper(){highReads++;Trace.hit(6);return high;}
    }
    static Comparable call(int route,Comparable seed,Range range) throws Throwable {
        if(route==0)return Client.call(seed,range);
        if(route==1)return RecursiveRangeResultCaller.call(seed,range);
        if(route==2)try{return (Comparable)Provider.class.getMethod("clamp",Comparable.class,Range.class).invoke(null,seed,range);}catch(InvocationTargetException e){throw e.getCause();}
        return (Comparable)MethodHandles.publicLookup().findStatic(Provider.class,"clamp",MethodType.methodType(Comparable.class,Comparable.class,Range.class)).invokeExact(seed,range);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getMethod("clamp",Comparable.class,Range.class);TypeVariable<?> t=method.getTypeParameters()[0];
        require(method.getGenericReturnType().equals(t)&&method.getGenericParameterTypes()[0].equals(t));
        ParameterizedType bound=(ParameterizedType)t.getBounds()[0];require(bound.getRawType()==Comparable.class&&((WildcardType)bound.getActualTypeArguments()[0]).getLowerBounds()[0].equals(t));
        ParameterizedType rangeType=(ParameterizedType)method.getGenericParameterTypes()[1];require(rangeType.getRawType()==Range.class&&rangeType.getActualTypeArguments()[0].equals(t));
        for(int attempt=0;attempt<2;attempt++){
            lowReads=highReads=comparisons=0;Trace.events=0;Trace.first=Trace.second=0;Trace.low=Trace.high=Trace.seenSeed=null;Trace.seenRange=null;
            failure=new RuntimeException("owned range failure");Trace.failure=failure;Trace.failAt=mode>=11?mode-10:0;
            long seedValue=mode==1?-5:mode==2?20:mode==9?Long.MIN_VALUE:mode==10?Long.MAX_VALUE:5+attempt;
            low=mode==5?null:mode==7?"wrong low":new Value(mode==9||mode==10?Long.MIN_VALUE:0);
            high=mode==6?null:mode==8?"wrong high":new Value(mode==9||mode==10?Long.MAX_VALUE:10);
            seed=mode==3?null:new Value(seedValue);Range range=mode==4?null:new Band();
            Comparable result=null;Throwable caught=null;try{result=call(route,seed,range);}catch(Throwable e){caught=e;}
            int length=mode>=11?mode-10:mode==1?5:mode==3||mode==7?3:mode==4?1:mode==5?4:mode==6?8:mode==8?7:9;
            long events=0;for(int i=1;i<=length;i++)events=events*10+i;
            require(Trace.events==events&&Trace.seenSeed==seed&&Trace.seenRange==range);
            require(lowReads==(length>=2?1:0)&&highReads==(length>=6?1:0)&&comparisons==(length>=4?1:0)+(length>=8?1:0));
            require(Trace.low==(length>=3?low:null)&&Trace.high==(length>=7?high:null));
            int first=length>=5?Long.compare(seedValue,((Value)low).number):0;
            int second=length>=9?Long.compare(seedValue,((Value)high).number):0;
            require(Trace.first==first&&Trace.second==second);
            if(mode>=11)require(caught==failure);
            else if(mode==3||mode==4||mode==5||mode==6)require(caught instanceof NullPointerException);
            else if(mode==7||mode==8)require(caught instanceof ClassCastException);
            else require(caught==null&&result==(mode==1?low:mode==2?high:seed));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
