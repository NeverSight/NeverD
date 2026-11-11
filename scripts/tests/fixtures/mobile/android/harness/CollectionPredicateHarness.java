import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class CollectionPredicateHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned collection predicate"); }
    static boolean call(int route,List values,Predicate predicate) throws Throwable {
        try {
            if(route==0)return Client.call(values,predicate);
            if(route==1)return CollectionPredicateCaller.call(values,predicate);
            if(route==2)return (Boolean)Provider.class.getMethod("remove",List.class,Predicate.class).invoke(null,values,predicate);
            return (boolean)MethodHandles.publicLookup().findStatic(Provider.class,"remove",
                MethodType.methodType(boolean.class,List.class,Predicate.class)).invokeExact(values,predicate);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getMethod("remove",List.class,Predicate.class);
        TypeVariable<?> formal=method.getTypeParameters()[0];
        require(method.getTypeParameters().length==1 && formal.getBounds()[0]==Object.class && method.getReturnType()==boolean.class);
        ParameterizedType list=(ParameterizedType)method.getGenericParameterTypes()[0];
        require(list.getRawType()==List.class && list.getActualTypeArguments()[0].equals(formal));
        Type[] slots=((ParameterizedType)method.getGenericParameterTypes()[1]).getActualTypeArguments();
        require(((WildcardType)slots[0]).getLowerBounds()[0].equals(formal));
        require(((WildcardType)slots[1]).getUpperBounds()[0]==Boolean.class);
        String full="1"+"7826394"+"78264"+"7826394"+"78264"+"75";
        String[] events={full,"1"+"78264"+"78264"+"78264"+"78264"+"75",
            "1"+"7826394"+"7826394"+"7826394"+"7826394"+"75","1","1782",
            "178263947826","178263947826","1782639","1782639478","178263947",
            "1","1782","178263","17826394",full,"175"};
        int[] hasCounts={5,5,5,0,1,2,2,1,2,2,0,1,1,1,5,1};
        int[] nextCounts={4,4,4,0,1,2,2,1,2,1,0,1,1,1,4,0};
        int[] predicateCounts={4,4,4,0,0,2,2,1,1,1,0,0,1,1,4,0};
        int[] removeCounts={2,0,4,0,0,1,1,1,1,1,0,0,0,1,2,0};
        RuntimeException original=new UnsupportedOperationException("owned iterator failure");
        for(int attempt=0;attempt<2;attempt++) {
            int[] a={7},b={11},c={13};Object[] input={a,b,c,null};
            int[] observed={0,0,0,0,0};
            ArrayList<Object> values=new ArrayList<Object>() {
                public Iterator<Object> iterator() {
                    observed[4]++;
                    Iterator<Object> delegate=super.iterator();
                    return new Iterator<Object>() {
                        public boolean hasNext() { observed[0]++;Trace.hit(7);if(mode==9&&observed[0]==2)throw original;return delegate.hasNext(); }
                        public Object next() { observed[1]++;Trace.hit(8);if(mode==8&&observed[1]==2)throw original;return delegate.next(); }
                        public void remove() { observed[3]++;Trace.hit(9);if(mode==7)throw original;delegate.remove(); }
                    };
                }
            };
            if(mode!=15)Collections.addAll(values,input);
            Predicate<Object,Boolean> predicate=value->{
                int index=observed[2]++;require(value==input[index]);Trace.hit(6);
                if(mode==5&&index==1)throw original;
                if(value!=null)((int[])value)[0]++;
                if(mode==6&&index==1)return null;
                return mode==1?Boolean.FALSE:mode==2?Boolean.TRUE:index==0||index==2?Boolean.TRUE:Boolean.FALSE;
            };
            Trace.events=0;Trace.current=null;Trace.failure=original;
            Trace.failAt=mode>=10&&mode<=14?mode-9:0;
            Throwable failure=null;boolean result=false;
            try { result=call(route,mode==3?null:values,mode==4?null:predicate); } catch(Throwable error) { failure=error; }
            long expected=0;for(char digit:events[mode].toCharArray())expected=expected*10+(digit-'0');
            require(Trace.events==expected && observed[0]==hasCounts[mode] && observed[1]==nextCounts[mode]
                && observed[2]==predicateCounts[mode] && observed[3]==removeCounts[mode]);
            require(observed[4]==(mode==3||mode==10?0:1));
            if(mode==0||mode==1||mode==2||mode==15)require(failure==null && result==(mode==0||mode==2));
            else if(mode==3||mode==4||mode==6)require(failure instanceof NullPointerException);
            else require(failure==original);
            boolean all=mode==0||mode==1||mode==2||mode==14;
            boolean partialRemoved=mode==5||mode==6||mode==8||mode==9||mode==13;
            Object[] remaining=mode==15||mode==2?new Object[]{}:all&&mode!=1?new Object[]{b,null}:
                partialRemoved?new Object[]{b,c,null}:input;
            require(values.size()==remaining.length);
            for(int i=0;i<remaining.length;i++)require(values.get(i)==remaining[i]);
            require(a[0]==7+(predicateCounts[mode]>0?1:0));
            require(b[0]==11+(all||mode==6?1:0));
            require(c[0]==13+(all?1:0));
            Object current=all?null:mode==5||mode==6?b:mode==3||mode==10||mode==15?null:a;
            require(Trace.current==current);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
