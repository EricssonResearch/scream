function plot_thp_delay(a,id,Tlim,maxThp,maxDelay)
% This function plots the thorughput
% RTT and estimated queue delay
% Parameters:
%  a        : log file from SCReAM BW test tool
%             imported with the command
%             a = load(<logfile>);
%             where <logfile> is the name of the log file
%  id       : stream id
%  Tlim     : xmin and xmax limits [s], e.g. [0 100]
%  maxThp   : Max thorughput [Mbps]
%  maxDelay : Max delay [s]
%
% The script can be used with matlab or octave
%
ix = find(a(:,8)==id);

T = a(ix,1);
T = T-T(1);

subplot(4,1,1);
plot(T,filter(1,1,a(ix,13))/1e6,T,a(ix,12)/1e6,T,a(ix,14)/1e6,T,a(ix,18)/1e6,T,min(maxThp/10,a(ix,15)/1e3-1),'.k');
set(gca,'FontSize',12);grid on;
set(gca,'XTickLabel',[]);grid on;
title('Throughput [Mbps]');
legend('Transmitted','RTP','ACKed','Target','Loss')
xlim(Tlim);
ylim([0 maxThp]);

subplot(4,1,2);
plot(T,a(ix,5)/1e3,'-r',T,a(ix,4)/1e3,'.-b');
set(gca,'FontSize',12);grid on;
set(gca,'XTickLabel',[]);grid on;
title('CWND(B) and bytes in flight(R) [kByte]');
xlim(Tlim);

subplot(4,1,3);
x = a(ix,11)./a(ix,10)*100;
plot(T,x);
mean(x)
set(gca,'FontSize',12);grid on;
set(gca,'XTickLabel',[]);grid on;
title('Congestion mark [%]');
xlim(Tlim);
ylim([0 110]);

subplot(4,1,4);
plot(T,a(ix,2),'.-',T,a(ix,19),'.-','linewidth',1);ylim([0 maxDelay]);
set(gca,'FontSize',12);grid on;
title('Network queue(B) and RTT(R) [s]');
xlim(Tlim);

xlabel('T [s]');
xlim(Tlim);
end

