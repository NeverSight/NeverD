import fixture.*;
import java.util.*;
public final class GuardedClosedDelegationHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned guarded closed delegation");}
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int mask=mode<16?mode:mode<30?15:mode<44?0:15;
        int fail=mode<16||mode==44?0:(mode<30?mode-16:mode-30)%7+1;
        boolean error=mode>=23&&mode<30||mode>=37&&mode<44;
        long[] values={Long.MIN_VALUE,Long.MAX_VALUE,-1L,0L};
        int[] floats={0,0x80000000,0x7f800000,0x7fc12345};
        for(int attempt=0;attempt<2;attempt++){
            Trace.parentPartial=null;Trace.corePartial=null;Trace.firstRead=null;Trace.secondRead=null;
            Trace.firstSeen=null;Trace.secondSeen=null;Trace.wideRead=0;Trace.wideSeen=0;
            Trace.scaleRead=0;Trace.scaleSeen=0;Trace.events=0;Trace.fail=fail;
            Trace.failure=error?new AssertionError("owned error"):new IllegalStateException("owned runtime");
            Object payload=new Object();
            Left left=new GuardedClosedDelegationProbe.LeftChild(payload);
            Right right=new GuardedClosedDelegationProbe.RightChild(payload);
            List first=new ArrayList(),second=new ArrayList(),defaultFirst=new ArrayList(),defaultSecond=new ArrayList();
            int variant=(mode+attempt)%4;
            if(variant==0){first.add(left);second.add(right);defaultFirst.add(left);defaultSecond.add(right);}
            else if(variant==1){first.add(null);second.add(null);defaultFirst.add(null);defaultSecond.add(null);}
            else if(variant==2){first=null;second=null;defaultFirst=null;defaultSecond=null;}
            else {first.add(payload);first.add(right);second=first;defaultFirst.add(payload);defaultFirst.add(left);defaultSecond=defaultFirst;}
            Trace.firstValue=defaultFirst;Trace.secondValue=defaultSecond;
            long wide=values[(mode+attempt)%4];float scale=Float.intBitsToFloat(floats[(mode+attempt)%4]);
            Trace.wideValue=values[(mode+attempt+1)%4]^0x91b7c5d3e2f0486aL;
            Trace.scaleValue=Float.intBitsToFloat(floats[(mode+attempt+2)%4]);
            List<?> selectedFirst=(mask&1)!=0?defaultFirst:first,selectedSecond=(mask&2)!=0?defaultSecond:second;
            long selectedWide=(mask&4)!=0?Trace.wideValue:wide;
            float selectedScale=(mask&8)!=0?Trace.scaleValue:scale;
            boolean[] visited=new boolean[8];long events=0;boolean expectedFailure=false;
            for(int step=1;step<=7;step++){
                if(step<=4&&(mask&(1<<(step-1)))==0)continue;
                visited[step]=true;events=events*10+step;
                if(fail==step){expectedFailure=true;break;}
            }
            Core result=null;Throwable failure=null;
            try{result=GuardedClosedDelegationCaller.make(route,mode,first,second,wide,scale,mask);}catch(Throwable e){failure=e;}
            require(Trace.events==events&&failure==(expectedFailure?Trace.failure:null));
            require(Trace.firstRead==(visited[1]?defaultFirst:null)&&Trace.secondRead==(visited[2]?defaultSecond:null));
            require(Trace.wideRead==(visited[3]?Trace.wideValue:0));
            require(Float.floatToRawIntBits(Trace.scaleRead)==Float.floatToRawIntBits(visited[4]?Trace.scaleValue:0));
            require((Trace.parentPartial!=null)==visited[5]);
            if(visited[5]){
                require(Trace.parentPartial instanceof Core);Core partial=(Core)Trace.parentPartial;
                require(partial.first==(visited[7]?selectedFirst:null)&&partial.second==(visited[7]?selectedSecond:null));
                require(partial.wide==(visited[7]?selectedWide:0));
                require(Float.floatToRawIntBits(partial.scale)==Float.floatToRawIntBits(visited[7]?selectedScale:0));
                if(visited[7]&&selectedFirst!=null)require(((List<?>)partial.first).get(0)==selectedFirst.get(0));
                if(visited[7]&&selectedSecond!=null)require(((List<?>)partial.second).get(0)==selectedSecond.get(0));
                require(Trace.corePartial==(!expectedFailure?partial:null)&&result==(!expectedFailure?partial:null));
            }else require(result==null&&Trace.corePartial==null);
            require(Trace.firstSeen==(visited[7]?selectedFirst:null)&&Trace.secondSeen==(visited[7]?selectedSecond:null));
            require(Trace.wideSeen==(visited[7]?selectedWide:0));
            require(Float.floatToRawIntBits(Trace.scaleSeen)==Float.floatToRawIntBits(visited[7]?selectedScale:0));
            require(left.payload==payload&&right.payload==payload);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events+",wide="+Long.toHexString(Trace.wideSeen)+",scale="+Integer.toHexString(Float.floatToRawIntBits(Trace.scaleSeen)));
        }
    }
}
