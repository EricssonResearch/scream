function plot_txrx(a,Tlim,maxDelay)
  

T = a(:,1);
T = T-T(1);
rxtx = a(:,5);
ix = find(a(:,5)>0);
rxtxmin= min(rxtx(ix));
rxtx(ix) = rxtx(ix)-rxtxmin;
ix = find(rxtx < 0);
if length(ix) > 0
plot(T,rxtx,T(ix),maxDelay/10,'ro'); 
else
plot(T,rxtx); 
end    
grid;
xlim(Tlim); ylim([0 maxDelay]);
set(gca,'FontSize',12);
xlabel('T[s]');
legend('RX-TX delay','Loss')
title('RX-TX delay [s]');

end