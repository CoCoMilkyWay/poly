import { BigInt, Address, BigDecimal } from "@graphprotocol/graph-ts";
import {
  ConditionPreparation,
  TransferSingle,
  TransferBatch,
  ConditionResolution,
} from "../generated/CTF/CTF";
import {
  NetUserBalance,
  TokenIdCondition,
  UserBalance,
  Condition,
} from "../generated/schema";
import {
  usdcAddress,
  AddressZero,
  negRiskAdapterAddress,
  wrappedUsdcAddress,
} from "./utils/constants";
import { calculatePositionIds } from "./utils/ctf-utls";

export function handleConditionPreparation(event: ConditionPreparation): void {
  if (event.params.outcomeSlotCount.equals(new BigInt(2))) {
    // only handle binary case
    return;
  }

  let conditionId = event.params.conditionId;

  let condition = new Condition(conditionId.toHexString());
  condition.save();

  // if the oracle is the negRiskAdapter, then the collateral is wrapped USDC
  // otherwise it's a standard market collateralized by USDC
  const collateralAddress = event.params.oracle.equals(
    Address.fromString(negRiskAdapterAddress)
  )
    ? wrappedUsdcAddress
    : usdcAddress;

  const positions = calculatePositionIds(
    event.address.toHexString(),
    event.params.conditionId.toHexString(),
    collateralAddress,
    event.params.outcomeSlotCount.toI32()
  );

  let entityOne = new TokenIdCondition(positions[0].toString());
  let entityTwo = new TokenIdCondition(positions[1].toString());

  entityOne.condition = conditionId.toHexString();
  entityOne.complement = positions[1].toString();
  entityOne.outcomeIndex = BigInt.fromI32(0);

  entityTwo.condition = conditionId.toHexString();
  entityTwo.complement = positions[0].toString();
  entityTwo.outcomeIndex = BigInt.fromI32(1);

  entityOne.save();
  entityTwo.save();

  // log.info(`registered entities with condition {} and positionIds {} and {}`, [
  //   conditionId.toHexString(),
  //   positions[0].toString(),
  //   positions[1].toString(),
  // ]);
}

export function handleConditionResolution(event: ConditionResolution): void {
  let conditionId = event.params.conditionId.toHexString();
  let condition = Condition.load(conditionId);
  if (condition == null) {
    return;
  }

  let payoutNumerators = event.params.payoutNumerators;
  let payoutDenominator = BigInt.fromI32(0);
  for (let i = 0; i < payoutNumerators.length; i += 1) {
    payoutDenominator = payoutDenominator.plus(payoutNumerators[i]);
  }
  let payoutDenominatorDec = payoutDenominator.toBigDecimal();
  let payouts = new Array<BigDecimal>(payoutNumerators.length);
  for (let i = 0; i < payouts.length; i += 1) {
    payouts[i] = payoutNumerators[i].divDecimal(payoutDenominatorDec);
  }
  condition.payouts = payouts;
  condition.save();
}

function _setNetPosition(
  user: Address,
  condition: string,
  tokenOne: string,
  tokenTwo: string
): void {
  let positiveBalanceAsset: string;
  let positiveBalanceAmount: BigInt;
  let tokenOneBalance = UserBalance.load(user.toHexString() + "-" + tokenOne);
  let tokenTwoBalance = UserBalance.load(user.toHexString() + "-" + tokenTwo);

  if (tokenOneBalance == null) {
    positiveBalanceAsset = tokenTwo;
    positiveBalanceAmount = tokenTwoBalance!.balance;
  } else if (tokenTwoBalance == null) {
    positiveBalanceAsset = tokenOne;
    positiveBalanceAmount = tokenOneBalance!.balance;
  } else {
    if (tokenOneBalance!.balance > tokenTwoBalance!.balance) {
      positiveBalanceAsset = tokenOne;
      positiveBalanceAmount = tokenOneBalance!.balance.minus(
        tokenTwoBalance!.balance
      );
    } else {
      positiveBalanceAsset = tokenTwo;
      positiveBalanceAmount = tokenTwoBalance!.balance.minus(
        tokenOneBalance!.balance
      );
    }
  }

  let netUserBalance = NetUserBalance.load(
    user.toHexString() + "-" + condition
  );

  if (netUserBalance == null) {
    netUserBalance = new NetUserBalance(user.toHexString() + "-" + condition);
    netUserBalance.user = user.toHexString();
    netUserBalance.asset = positiveBalanceAsset;
    netUserBalance.balance = positiveBalanceAmount;
  } else {
    netUserBalance.asset = positiveBalanceAsset;
    netUserBalance.balance = positiveBalanceAmount;
  }

  netUserBalance.save();
}

function _adjustSenderBalance(
  sender: Address,
  tokenCondition: TokenIdCondition,
  amount: BigInt
): void {
  if (
    sender != Address.fromString(AddressZero) &&
    amount.gt(BigInt.fromI32(0))
  ) {
    let senderBalance = UserBalance.load(
      sender.toHexString() + "-" + tokenCondition.id.toString()
    ) as UserBalance; // sender will always have balance so we don't need to concern ourselves with null case
    senderBalance.balance = senderBalance.balance.minus(amount);
    senderBalance.save();

    _setNetPosition(
      sender,
      tokenCondition.condition,
      tokenCondition.id,
      tokenCondition.complement
    );
  }
}

function _adjustReceiverBalance(
  receiver: Address,
  tokenCondition: TokenIdCondition,
  amount: BigInt
): void {
  if (
    receiver != Address.fromString(AddressZero) &&
    amount.gt(BigInt.fromI32(0))
  ) {
    let receiverBalance = UserBalance.load(
      receiver.toHexString() + "-" + tokenCondition.id.toString()
    );

    if (receiverBalance == null) {
      receiverBalance = new UserBalance(
        receiver.toHexString() + "-" + tokenCondition.id.toString()
      );
      receiverBalance.user = receiver.toHexString();
      receiverBalance.asset = tokenCondition.id.toString();
      receiverBalance.balance = amount;
    } else {
      receiverBalance.balance = receiverBalance.balance.plus(amount);
    }
    receiverBalance.save();

    _setNetPosition(
      receiver,
      tokenCondition.condition,
      tokenCondition.id,
      tokenCondition.complement
    );
  }
}

export function handleTransferSingle(event: TransferSingle): void {
  const sender = event.params.from;
  const receiver = event.params.to;
  const tokenId = event.params.id;

  let tokenIdCondition = TokenIdCondition.load(tokenId.toString());

  if (tokenIdCondition == null) {
    return;
  }

  _adjustSenderBalance(sender, tokenIdCondition!, event.params.value);
  _adjustReceiverBalance(receiver, tokenIdCondition!, event.params.value);
}

export function handleTransferBatch(event: TransferBatch): void {
  const sender = event.params.from;
  const receiver = event.params.to;

  for (let i = 0; i < event.params.ids.length; i++) {
    const tokenId = event.params.ids[i];
    let tokenIdCondition = TokenIdCondition.load(tokenId.toString());

    if (tokenIdCondition == null) {
      // might be a >2 outcome condition we don't have
      return;
    }

    _adjustSenderBalance(sender, tokenIdCondition, event.params.values[i]);
    _adjustReceiverBalance(receiver, tokenIdCondition, event.params.values[i]);
  }
}
